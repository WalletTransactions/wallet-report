# Spec Delta

## ADDED Requirements

### Requirement: Признак долга в справочнике кошельков
Кошелёк в `/accounts` SHALL содержать `counterparty_uuid` (`null` — не
долговой), определяемый по живой строке `account_debt`. Остальные поля и
порядок строк не меняются.

#### Scenario: Долговой кошелёк в справочнике
- **WHEN** у кошелька есть живая строка `account_debt`
- **THEN** его запись содержит `counterparty_uuid` контрагента

#### Scenario: Бот отбирает кошельки для трат
- **WHEN** wallet-telegram запрашивает `/accounts`
- **THEN** по `counterparty_uuid != null` он может исключить долговые кошельки
