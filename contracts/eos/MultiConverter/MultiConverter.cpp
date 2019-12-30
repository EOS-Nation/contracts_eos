
/**
 *  @file
 *  @copyright defined in ../../../LICENSE
 */

#include "../Token/Token.hpp"
#include "MultiConverter.hpp"

ACTION MultiConverter::create(name owner, symbol_code token_code, double initial_supply) {
    require_auth(owner);

    symbol token_symbol = symbol(token_code, DEFAULT_TOKEN_PRECISION);
    double maximum_supply = DEFAULT_MAX_SUPPLY;

    settings settings_table(get_self(), get_self().value);
    const auto& st = settings_table.get("settings"_n.value, "settings do not exist");

    converters converters_table(get_self(), token_symbol.code().raw());
    const auto& converter = converters_table.find(token_symbol.code().raw());
  
    check(converter == converters_table.end(), "converter for the given symbol already exists");
    check(initial_supply > 0, "must have a non-zero initial supply");
    check(initial_supply / maximum_supply <= MAX_INITIAL_MAXIMUM_SUPPLY_RATIO , "the ratio between initial and max supply is too big");
    
    converters_table.emplace(owner, [&](auto& c) {
        c.currency = token_symbol;
        c.owner = owner;
        c.enabled = false;
        c.launched = false;
        c.stake_enabled = false;
        c.fee = 0;
    });

    asset initial_supply_asset = asset(initial_supply * pow(10, token_symbol.precision()) , token_symbol);
    asset maximum_supply_asset = asset(maximum_supply * pow(10, token_symbol.precision()) , token_symbol);

    action( 
        permission_level{ st.multi_token, "active"_n },
        st.multi_token, "create"_n, 
        make_tuple(get_self(), maximum_supply_asset) 
    ).send();

    action( 
        permission_level{ get_self(), "active"_n },
        st.multi_token, "issue"_n, 
        make_tuple(get_self(), initial_supply_asset, string("setup"))
    ).send();

    action(
        permission_level{ get_self(), "active"_n },
        st.multi_token, "transfer"_n,
        make_tuple(get_self(), owner, initial_supply_asset, string("setup"))
    ).send();
}

ACTION MultiConverter::setenabled(bool enabled) {   
    require_auth(get_self());
    settings settings_table(get_self(), get_self().value);
    const auto& st = settings_table.get("settings"_n.value, "set token contract first");
    check(enabled != st.active, "setting same value as before");
    settings_table.modify(st, same_payer, [&](auto& s) {		
        s.active = enabled;
    });
}

ACTION MultiConverter::setmaxfee(uint64_t maxfee) {
    require_auth(get_self());
   
    settings settings_table(get_self(), get_self().value);
    const auto& st = settings_table.get("settings"_n.value, "set token contract first");

    string error = string("max fee must be lower or equal to ") + std::to_string(MAX_FEE);
    check(maxfee <= MAX_FEE, error.c_str());
    check(maxfee != st.max_fee, "setting same value as before");
    settings_table.modify(st, same_payer, [&](auto& s) {		
        s.max_fee = maxfee;
    });
}

ACTION MultiConverter::setstaking(name staking) {
    check(is_account(staking), "invalid staking account");
    require_auth(get_self());    
    
    settings settings_table(get_self(), get_self().value);
    const auto& st = settings_table.get("settings"_n.value, "set token contract first");    
    
    check(!is_account(st.staking), "staking contract already set");
    
    settings_table.modify(st, same_payer, [&](auto& s) {		
        s.staking = staking;
    });
}

ACTION MultiConverter::setmultitokn(name multi_token) {   
    check(is_account(multi_token), "invalid multi-token account");
    require_auth(get_self());

    settings settings_table(get_self(), get_self().value);
    auto st = settings_table.find("settings"_n.value);

    check(st == settings_table.end(), "can only call setmultitokn once");
    
    settings_table.emplace(get_self(), [&](auto& s) {		
        s.active = false;
        s.multi_token = multi_token;
    });
}

ACTION MultiConverter::enablecnvrt(symbol_code currency, bool enabled) {
    converters converters_table(get_self(), currency.raw());
    settings settings_table(get_self(), get_self().value);
    
    const auto& st = settings_table.get("settings"_n.value, "settings do not exist");
    const auto& converter = converters_table.get(currency.raw(), "converter does not exist");
    require_auth(converter.owner);
    bool prevEnabled = converter.enabled;
    check(enabled != prevEnabled, "setting same value as before");
    converters_table.modify(converter, same_payer, [&](auto& c) {
        c.enabled = enabled;
        c.launched = true;
    });

    if (!prevEnabled) {
        reserves reserves_table(get_self(), currency.raw());
        
        asset supply = get_supply(st.multi_token, converter.currency.code());
        auto current_smart_supply = supply.amount / pow(10, converter.currency.precision());

        for (auto& reserve : reserves_table) {
            auto reserve_balance = reserve.balance.amount / pow(10, reserve.balance.symbol.precision()); 
            EMIT_PRICE_DATA_EVENT(currency, current_smart_supply, 
                                  reserve.contract, reserve.balance.symbol.code(), 
                                  reserve_balance, reserve.ratio);
        }
    }    
}

ACTION MultiConverter::enablestake(symbol_code currency, bool enabled) {
    converters converters_table(get_self(), currency.raw());
    settings settings_table(get_self(), get_self().value);
    const auto& st = settings_table.get("settings"_n.value, "settings do not exist");
    const auto& converter = converters_table.get(currency.raw(), "converter does not exist");
    require_auth(converter.owner);

    check(is_account(st.staking), "set staking account before enabling staking");
    check(enabled != converter.stake_enabled, "setting same value as before");
    
    converters_table.modify(converter, same_payer, [&](auto& c) {
        c.stake_enabled = enabled;        
    });
}

ACTION MultiConverter::updateowner(symbol_code currency, name new_owner) {
    converters converters_table(get_self(), currency.raw());
    const auto& converter = converters_table.get(currency.raw(), "converter does not exist");

    require_auth(converter.owner);
    check(is_account(new_owner), "new owner is not an account");
    check(new_owner != converter.owner, "setting same owner as before");
    converters_table.modify(converter, same_payer, [&](auto& c) {
        c.owner = new_owner;
    });   
}

ACTION MultiConverter::updatefee(symbol_code currency, uint64_t fee) {
    settings settings_table(get_self(), get_self().value);
    converters converters_table(get_self(), currency.raw());
    
    const auto& st = settings_table.get("settings"_n.value, "settings do not exist");
    const auto& converter = converters_table.get(currency.raw(), "converter does not exist");

    if (converter.stake_enabled)
        require_auth(st.staking);
    else
        require_auth(converter.owner);
    
    check(fee <= st.max_fee, "fee must be lower or equal to the maximum fee");
    if (converter.fee != fee) { 
        uint64_t prevFee = converter.fee;
        converters_table.modify(converter, same_payer, [&](auto& c) {
            c.fee = fee;
        });
        EMIT_CONVERSION_FEE_UPDATE_EVENT(currency, prevFee, fee);
    }
}

ACTION MultiConverter::setreserve(symbol_code converter_currency_code, symbol currency, name contract, bool sale_enabled, uint64_t ratio) {
    converters converters_table(get_self(), converter_currency_code.raw());
    const auto& converter = converters_table.get(converter_currency_code.raw(), "converter does not exist");
    require_auth(converter.owner);
    
    settings settings_table(get_self(), get_self().value);
    const auto& st = settings_table.get("settings"_n.value, "settings do not exist");

    string error = string("ratio must be between 1 and ") + std::to_string(MAX_RATIO);
    check(ratio > 0 && ratio <= MAX_RATIO, error.c_str());
    
    check(is_account(contract), "token's contract is not an account");
    check(currency.is_valid(), "invalid reserve symbol");
    
    reserves reserves_table(get_self(), converter_currency_code.raw());
    auto reserve = reserves_table.find(currency.code().raw());
    asset smart_supply = get_supply(st.multi_token, converter.currency.code());

    if (reserve != reserves_table.end()) {
        check(reserve->contract == contract, "cannot update the reserve contract name");
        reserves_table.modify(reserve, get_self(), [&](auto& r) {
            r.ratio = ratio;
            r.sale_enabled = sale_enabled;
        });
        auto reserve_balance = reserve->balance.amount / pow(10, currency.precision()); 
        EMIT_PRICE_DATA_EVENT(converter_currency_code, 
                              smart_supply.amount / pow(10, currency.precision()), 
                              reserve->contract, currency.code(), reserve_balance, ratio);
    } else 
        reserves_table.emplace(converter.owner, [&](auto& r) {
            r.contract = contract;
            r.ratio = ratio;
            r.sale_enabled = sale_enabled;
            r.balance = asset(0, currency);
        });
    
    double total_ratio = 0.0;
    for (auto& reserve : reserves_table)
        total_ratio += reserve.ratio;
    
    check(total_ratio <= MAX_RATIO, "total ratio cannot exceed the maximum ratio");
}

ACTION MultiConverter::delreserve(symbol_code converter, symbol_code currency) {
    converters converters_table(get_self(), converter.raw());
    const auto& cnvrt = converters_table.get(converter.raw(), "converter does not exist");
    require_auth(cnvrt.owner);

    reserves reserves_table(get_self(), converter.raw());
    const auto& rsrv = reserves_table.get(currency.raw(), "reserve not found");
    check(!rsrv.balance.amount, "may delete only empty reserves");

    reserves_table.erase(rsrv);
}

ACTION MultiConverter::close(symbol_code converter_currency_code) {
    converters converters_table(get_self(), converter_currency_code.raw());
    reserves reserves_table(get_self(), converter_currency_code.raw());

    const auto& converter = converters_table.get(converter_currency_code.raw(), "converter does not exist");
    require_auth(converter.owner);

    check(reserves_table.begin() == reserves_table.end(), "delete reserves first");
    
    converters_table.erase(converter);
}

ACTION MultiConverter::fund(name sender, asset quantity) {
    require_auth(sender);
    check(quantity.is_valid() && quantity.amount > 0, "invalid quantity");
    
    settings settings_table(get_self(), get_self().value);
    converters converters_table(get_self(), quantity.symbol.code().raw());
    const auto& st = settings_table.get("settings"_n.value, "settings do not exist");
    const auto& converter = converters_table.get(quantity.symbol.code().raw(), "converter does not exist");
    
    check(converter.currency == quantity.symbol, "symbol mismatch");
    check(converter.enabled, "cannot fund when converter is disabled");
    asset supply = get_supply(st.multi_token, quantity.symbol.code());
    reserves reserves_table(get_self(), quantity.symbol.code().raw());
    
    double total_ratio = 0.0;
    double smart_amount = quantity.amount;
    double current_smart_supply = supply.amount;
    double percent = smart_amount / current_smart_supply;

    for (auto& reserve : reserves_table) {
        total_ratio += reserve.ratio;
        asset reserve_amount = asset(reserve.balance.amount * percent + 1, reserve.balance.symbol);
        
        mod_account_balance(sender, quantity.symbol.code(), -reserve_amount);
        mod_reserve_balance(quantity.symbol, reserve_amount);
    }
    check(total_ratio == MAX_RATIO, "total ratio must add up to 100%");
    action( // issue new smart tokens to the issuer
        permission_level{ get_self(), "active"_n },
        st.multi_token, "issue"_n, 
        make_tuple(get_self(), quantity, string("fund"))
    ).send();
    action(
        permission_level{ get_self(), "active"_n },
        st.multi_token, "transfer"_n,
        make_tuple(get_self(), sender, quantity, string("fund"))
    ).send();
}

void MultiConverter::liquidate(name sender, asset quantity) {
    settings settings_table(get_self(), get_self().value);
    const auto& st = settings_table.get("settings"_n.value, "settings do not exist");
    check(get_first_receiver() == st.multi_token, "bad origin for this transfer");
    
    asset supply = get_supply(st.multi_token, quantity.symbol.code());
    reserves reserves_table(get_self(), quantity.symbol.code().raw());
    
    double total_ratio = 0.0;
    double smart_amount = quantity.amount;
    double current_smart_supply = supply.amount;
    double percent = smart_amount / current_smart_supply;

    for (auto& reserve : reserves_table) {
        total_ratio += reserve.ratio;
        asset reserve_amount = asset(reserve.balance.amount * percent, reserve.balance.symbol);
        
        check(reserve_amount.amount > 0, "cannot liquidate amounts less than or equal to 0");
        mod_reserve_balance(quantity.symbol, -reserve_amount);
        action(
            permission_level{ get_self(), "active"_n },
            reserve.contract, "transfer"_n,
            make_tuple(get_self(), sender, reserve_amount, string("liquidation"))
        ).send();   
    }
    check(total_ratio == MAX_RATIO, "total ratio must add up to 100%");
    action( // remove smart tokens from circulation
        permission_level{ get_self(), "active"_n },
        st.multi_token, "retire"_n,
        make_tuple(quantity, string("liquidation"))
    ).send();
}

ACTION MultiConverter::withdraw(name sender, asset quantity, symbol_code converter_currency_code) {
    require_auth(sender);
    check(quantity.is_valid() && quantity.amount > 0, "invalid quantity");
    mod_balances(sender, -quantity, converter_currency_code, get_self());
}

void MultiConverter::mod_account_balance(name sender, symbol_code converter_currency_code, asset quantity) {
    accounts acnts(get_self(), sender.value);
    auto key = _by_cnvrt(quantity, converter_currency_code);
    auto index = acnts.get_index<"bycnvrt"_n >();
    auto itr = index.find(key);
    bool exists = itr != index.end();
    uint64_t balance = quantity.amount;
    if (quantity.amount < 0) {
        check(exists, "cannot withdraw non-existant deposit");
        check(itr->quantity >= -quantity, "insufficient balance");
    }
    if (exists) {
        index.modify(itr, same_payer, [&](auto& acnt) {
            acnt.quantity += quantity;
        });
        if (itr->is_empty()) 
            index.erase(itr);    
    } else 
        acnts.emplace(get_self(), [&](auto& acnt) {
            acnt.symbl = converter_currency_code;
            acnt.quantity = quantity;
            acnt.id = acnts.available_primary_key();
        });
}

void MultiConverter::mod_balances(name sender, asset quantity, symbol_code converter_currency_code, name code) {
    reserves reserves_table(get_self(), converter_currency_code.raw());
    const auto& reserve = reserves_table.get(quantity.symbol.code().raw(), "reserve doesn't exist");

    converters converters_table(get_self(), converter_currency_code.raw());
    const auto& converter = converters_table.get(converter_currency_code.raw(), "converter does not exist");

    if (quantity.amount > 0)
        check(code == reserve.contract, "wrong origin contract for quantity");
    else
        action(
            permission_level{ get_self(), "active"_n },
            reserve.contract, "transfer"_n,
            make_tuple(get_self(), sender, -quantity, string("withdrawal"))
        ).send();
    
    if (converter.launched)
        mod_account_balance(sender, converter_currency_code, quantity);
    else {
        check(sender == converter.owner, "only converter owner may fund/withdraw prior to activation");
        mod_reserve_balance(converter.currency, quantity);
    }
}

void MultiConverter::mod_reserve_balance(symbol converter_currency, asset value) {
    settings settings_table(get_self(), get_self().value);
    const auto& st = settings_table.get("settings"_n.value, "settings do not exist");
    asset supply = get_supply(st.multi_token, converter_currency.code());
    auto current_smart_supply = supply.amount / pow(10, converter_currency.precision());
    
    reserves reserves_table(get_self(), converter_currency.code().raw());
    auto& reserve = reserves_table.get(value.symbol.code().raw(), "reserve not found");
    check(reserve.balance.symbol == value.symbol, "incompatible symbols");
    reserves_table.modify(reserve, same_payer, [&](auto& r) {
        r.balance += value;
    });
    int64_t reserve_balance = reserve.balance.amount;
    check(reserve_balance >= 0, "insufficient amount in reserve");
    reserve_balance /= pow(10, reserve.balance.symbol.precision()); 
    EMIT_PRICE_DATA_EVENT(converter_currency.code(), current_smart_supply, 
                          reserve.contract, reserve.balance.symbol.code(), 
                          reserve_balance, reserve.ratio);
}

void MultiConverter::convert(name from, asset quantity, string memo, name code) {
    memo_structure memo_object = parse_memo(memo);
    check(memo_object.path.size() > 1, "invalid memo format");
    check(memo_object.converters[0].account == get_self(), "wrong converter");

    settings settings_table(get_self(), get_self().value);
    const auto& settings = settings_table.get("settings"_n.value, "settings do not exist");
    
    symbol_code converter_currency_code = symbol_code(memo_object.converters[0].sym);
    converters converters_table(get_self(), converter_currency_code.raw());
    const auto& converter = converters_table.get(converter_currency_code.raw(), "converter does not exist");

    check(settings.active && converter.enabled, "conversions are disabled");
    
    auto from_path_currency = quantity.symbol.code().raw();
    auto to_path_currency = symbol_code(memo_object.path[1].c_str()).raw();
    check(from_path_currency != to_path_currency, "cannot convert equivalent currencies");

    double from_amount = quantity.amount / pow(10, quantity.symbol.precision());
    const reserve_t& from_token = get_reserve(from_path_currency, converter);
    const reserve_t& to_token = get_reserve(to_path_currency, converter);

    check(to_token.sale_enabled, "'to' token purchases disabled");
    check(code == from_token.contract, "unknown 'from' contract");

    bool incoming_smart_token = from_token.balance.symbol.code().raw() == converter.currency.code().raw();
    bool outgoing_smart_token = to_token.balance.symbol.code().raw() == converter.currency.code().raw();

    if (!incoming_smart_token) // add to reserve
        mod_reserve_balance(converter.currency, quantity);    
    else 
        action(
            permission_level{ get_self(), "active"_n },
            settings.multi_token, "retire"_n,
            make_tuple(quantity, string("destroy on conversion"))
        ).send();

    if (outgoing_smart_token)
        check(memo_object.path.size() == 2, "smart token must be final currency");
 
    double to_return = calculate_return(from_token, to_token, from_amount, memo, converter, settings.multi_token);
    
    complete_convert(memo_object, to_token.contract, to_return, to_token.balance.symbol, converter.currency);
}

double MultiConverter::calculate_return(const reserve_t& from_token, const reserve_t& to_token, double from_amount, string memo, const converter_t& converter, name multi_token) {
    symbol from_symbol = from_token.balance.symbol;
    symbol to_symbol = to_token.balance.symbol;

    bool incoming_smart_token = from_symbol == converter.currency;    
    bool outgoing_smart_token = to_symbol == converter.currency;
    bool quick_conversion = !incoming_smart_token && !outgoing_smart_token && from_token.ratio == to_token.ratio;

    double current_from_balance = from_token.balance.amount / pow(10, from_symbol.precision());
    double current_to_balance = to_token.balance.amount / pow(10, to_symbol.precision());
        
    asset supply = get_supply(multi_token, converter.currency.code());
    double current_smart_supply = supply.amount / pow(10, converter.currency.precision());

    double smart_tokens = 0;
    double to_return = 0;
    
    if (incoming_smart_token)
        smart_tokens = from_amount;
    else if (quick_conversion)
        to_return = quick_convert(current_from_balance, from_amount, current_to_balance);
    else {
        smart_tokens = calculate_purchase_return(current_from_balance, from_amount, current_smart_supply, from_token.ratio);
        current_smart_supply += smart_tokens;
    }
    if (outgoing_smart_token)
        to_return = smart_tokens;
    
    else if (!quick_conversion) {
        to_return = calculate_sale_return(current_to_balance, smart_tokens, current_smart_supply, to_token.ratio);
        current_smart_supply -= smart_tokens;
    }
    uint8_t magnitude = incoming_smart_token || outgoing_smart_token ? 1 : 2;
    double fee = calculate_fee(to_return, converter.fee, magnitude);
    to_return -= fee;
    if (outgoing_smart_token)
        current_smart_supply -= fee;

    double formatted_total_fee_amount = to_fixed(fee, to_symbol.precision());
    double formatted_smart_supply = to_fixed(current_smart_supply, converter.currency.precision());
    
    EMIT_CONVERSION_EVENT(converter.currency.code(), memo, 
                          from_token.contract, from_symbol.code(), 
                          to_token.contract, to_symbol.code(), 
                          from_amount, to_return, formatted_total_fee_amount);
    
    if (!incoming_smart_token)
        EMIT_PRICE_DATA_EVENT(converter.currency.code(), formatted_smart_supply, 
                              from_token.contract, from_symbol.code(), 
                              current_from_balance + from_amount, (from_token.ratio / MAX_RATIO));
    if (!outgoing_smart_token)
        EMIT_PRICE_DATA_EVENT(converter.currency.code(), formatted_smart_supply, 
                              to_token.contract, to_symbol.code(), 
                              current_to_balance - to_return, (to_token.ratio / MAX_RATIO));  
    return to_return;
}

void MultiConverter::complete_convert(memo_structure memo_object, name to_token_contract, double to_return, symbol to_symbol, symbol converter_currency) {
    path new_path = memo_object.path;
    new_path.erase(new_path.begin(), new_path.begin() + 2);
    memo_object.path = new_path;
    
    auto new_memo = build_memo(memo_object);                         
    
    uint64_t to_amount = to_return * pow(10, to_symbol.precision());
    auto new_asset = asset(to_amount, to_symbol);

    if (to_symbol == converter_currency)
        action(
            permission_level{ get_self(), "active"_n },
            to_token_contract, "issue"_n,
            make_tuple(get_self(), new_asset, new_memo)
        ).send();
    else
        mod_reserve_balance(converter_currency, -new_asset); // subtract from reserve
        
    check(new_asset.amount > 0, "below min return");
    action(
        permission_level{ get_self(), "active"_n },
        to_token_contract, "transfer"_n,
        make_tuple(get_self(), BANCOR_NETWORK, new_asset, new_memo)
    ).send();
}

// returns a reserve object, can also be called for the smart token itself
const MultiConverter::reserve_t& MultiConverter::get_reserve(uint64_t symbl, const converter_t& converter) {
    settings settings_table(get_self(), get_self().value);
    const auto& st = settings_table.get("settings"_n.value, "settings do not exist");

    if (converter.currency.code().raw() == symbl) { // smart token incoming/outgoing, not actually a reserve, returned as such for semantics purposes only
        asset supply = get_supply(st.multi_token, converter.currency.code());
        static reserve_t temp_reserve = {
            st.multi_token, 0, 
            converter.enabled,
            supply
        };
        return temp_reserve;
    }
    reserves reserves_table(get_self(), converter.currency.code().raw());
    const auto& reserve = reserves_table.get(symbl, "reserve not found");
    return reserve;
}

// returns a token supply
asset MultiConverter::get_supply(name contract, symbol_code sym) {
    Token::stats statstable(contract, sym.raw());
    const auto& st = statstable.get(sym.raw(), "no stats found");
    return st.supply;
}

// given a token supply, reserve balance, ratio and a input amount (in the reserve token),
// calculates the return for a given conversion (in the smart token)
double MultiConverter::calculate_purchase_return(double balance, double deposit_amount, double supply, int64_t ratio) {
    double R(supply);
    double C(balance);
    double F(ratio / MAX_RATIO);
    double T(deposit_amount);
    double ONE(1.0);

    double E = -R * (ONE - pow(ONE + T / C, F));
    return E;
}

// given a token supply, reserve balance, ratio and a input amount (in the smart token),
// calculates the return for a given conversion (in the reserve token)
double MultiConverter::calculate_sale_return(double balance, double sell_amount, double supply, int64_t ratio) {
    double R(supply);
    double C(balance);
    double F(MAX_RATIO / ratio);
    double E(sell_amount);
    double ONE(1.0);

    double T = C * (ONE - pow(ONE - E/R, F));
    return T;
}

double MultiConverter::quick_convert(double balance, double in, double toBalance) {
    return in / (balance + in) * toBalance;
}

void MultiConverter::on_transfer(name from, name to, asset quantity, string memo) {    
    require_auth(from);
    // avoid unstaking and system contract ops mishaps
    if (to != get_self() || from == get_self() || 
        from == "eosio.ram"_n || from == "eosio.stake"_n || from == "eosio.rex"_n) return;

    check(quantity.is_valid() && quantity.amount > 0, "invalid quantity");
    
    const auto& splitted_memo = split(memo, ";");
    if (splitted_memo[0] == "fund")
        mod_balances(from, quantity, symbol_code(splitted_memo[1]), get_first_receiver());
    else if (splitted_memo[0] == "liquidate")
        liquidate(from, quantity);
    
    else {
        check(from == BANCOR_NETWORK, "converter can only receive from network contract");
        convert(from, quantity, memo, get_first_receiver()); 
    }
}

extern "C" void apply(uint64_t receiver, uint64_t code, uint64_t action) {
    if (action == "transfer"_n.value && code != receiver) 
        eosio::execute_action(eosio::name(receiver), eosio::name(code), &MultiConverter::on_transfer);

    if (code == receiver)
        switch (action) {
            EOSIO_DISPATCH_HELPER(MultiConverter, (create)(close)
            (setmultitokn)(setstaking)(setmaxfee)(setenabled)
            (updateowner)(updatefee)(enablecnvrt)(enablestake)
            (setreserve)(delreserve)(withdraw)(fund)) 
        }    
}
