[[eosio::action]]
void BancorConverter::setreserve(symbol_code converter_currency_code, symbol currency, name contract, uint64_t ratio) {
    converters converters_table(get_self(), get_self().value);
    const auto& converter = converters_table.get(converter_currency_code.raw(), "converter does not exist");
    require_auth(converter.owner);

    const string error = string("ratio must be between 1 and ") + std::to_string(MAX_RATIO);
    check(ratio > 0 && ratio <= MAX_RATIO, error.c_str());

    check(is_account(contract), "token's contract is not an account");
    check(currency.is_valid(), "invalid reserve symbol");

    reserves reserves_table(get_self(), converter_currency_code.raw());
    const auto reserve = reserves_table.find(currency.code().raw());
    check(reserve == reserves_table.end(), "reserve already exists");

    reserves_table.emplace(converter.owner, [&](auto& r) {
        r.contract = contract;
        r.ratio = ratio;
        r.balance = asset(0, currency);
    });

    double total_ratio = 0.0;
    for (auto& reserve : reserves_table)
        total_ratio += reserve.ratio;

    check(total_ratio <= MAX_RATIO, "total ratio cannot exceed the maximum ratio");
}

[[eosio::action]]
void BancorConverter::delreserve(symbol_code converter, symbol_code reserve) {
    check(!is_converter_active(converter),  "a reserve can only be deleted if it's converter is inactive");
    reserves reserves_table(get_self(), converter.raw());
    const auto& rsrv = reserves_table.get(reserve.raw(), "reserve not found");

    reserves_table.erase(rsrv);
}

[[eosio::action]]
void BancorConverter::fund(name sender, asset quantity) {
    require_auth(sender);
    check(quantity.is_valid() && quantity.amount > 0, "invalid quantity");

    settings settings_table(get_self(), get_self().value);
    converters converters_table(get_self(), get_self().value);
    const auto& st = settings_table.get();
    const auto& converter = converters_table.get(quantity.symbol.code().raw(), "converter does not exist");

    check(converter.currency == quantity.symbol, "symbol mismatch");
    const asset supply = get_supply(st.multi_token, quantity.symbol.code());
    reserves reserves_table(get_self(), quantity.symbol.code().raw());

    double total_ratio = 0.0;
    for (const auto& reserve : reserves_table)
        total_ratio += reserve.ratio;

    for (auto& reserve : reserves_table) {
        double amount = calculate_fund_cost(quantity.amount, supply.amount, reserve.balance.amount, total_ratio);
        asset reserve_amount = asset(ceil(amount), reserve.balance.symbol);

        mod_account_balance(sender, quantity.symbol.code(), -reserve_amount);
        mod_reserve_balance(quantity.symbol, reserve_amount);
    }

    // issue new smart tokens to the issuer
    Token::issue_action issue( st.multi_token, { get_self(), "active"_n });
    issue.send(get_self(), quantity, "fund");

    Token::transfer_action transfer( st.multi_token, { get_self(), "active"_n });
    transfer.send(get_self(), sender, quantity, "fund");
}

[[eosio::action]]
void BancorConverter::withdraw(name sender, asset quantity, symbol_code converter_currency_code) {
    require_auth(sender);
    check(quantity.is_valid() && quantity.amount > 0, "invalid quantity");
    mod_balances(sender, -quantity, converter_currency_code, get_self());
}

double BancorConverter::calculate_fund_cost(double funding_amount, double supply, double reserve_balance, double total_ratio) {
    check(supply > 0, "supply must be greater than zero");
    check(reserve_balance > 0, "reserve_balance must be greater than zero");
    check(total_ratio > 1 && total_ratio <= MAX_RATIO * 2, "total_ratio not in range");

    if (funding_amount == 0)
        return 0;

    if (total_ratio == MAX_RATIO)
        return reserve_balance * funding_amount / supply;

    return reserve_balance * (pow(((supply + funding_amount) / supply), (MAX_RATIO / total_ratio)) - 1.0);
}

void BancorConverter::liquidate(name sender, asset quantity) {
    settings settings_table(get_self(), get_self().value);
    const auto& st = settings_table.get();
    check(get_first_receiver() == st.multi_token, "bad origin for this transfer");

    const asset supply = get_supply(st.multi_token, quantity.symbol.code());
    reserves reserves_table(get_self(), quantity.symbol.code().raw());

    double total_ratio = 0.0;
    for (const auto& reserve : reserves_table)
        total_ratio += reserve.ratio;

    for (const auto& reserve : reserves_table) {
        double amount = calculate_liquidate_return(quantity.amount, supply.amount, reserve.balance.amount, total_ratio);
        check(amount > 0, "cannot liquidate amounts less than or equal to 0");

        asset reserve_amount = asset(amount, reserve.balance.symbol);

        mod_reserve_balance(quantity.symbol, -reserve_amount);
        Token::transfer_action transfer( reserve.contract, { get_self(), "active"_n });
        transfer.send(get_self(), sender, reserve_amount, "liquidation");
    }

    // remove smart tokens from circulation
    Token::retire_action retire( st.multi_token, { get_self(), "active"_n });
    retire.send(quantity, "liquidation");
}

double BancorConverter::calculate_liquidate_return(double liquidation_amount, double supply, double reserve_balance, double total_ratio) {
    check(supply > 0, "supply must be greater than zero");
    check(reserve_balance > 0, "reserve_balance must be greater than zero");
    check(liquidation_amount <= supply, "liquidation_amount must be less than or equal to the supply");
    check(total_ratio > 1 && total_ratio <= MAX_RATIO * 2, "total_ratio not in range");

    if (liquidation_amount == 0)
        return 0;

    if (liquidation_amount == supply)
        return reserve_balance;

    if (total_ratio == MAX_RATIO)
        return liquidation_amount * reserve_balance / supply;

    return reserve_balance * (1.0 - pow(((supply - liquidation_amount) / supply), (MAX_RATIO / total_ratio)));
}