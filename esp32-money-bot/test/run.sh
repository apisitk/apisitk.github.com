#!/bin/sh
# Host test: needs g++ and a checkout of ArduinoJson 7 (path in $ARDUINOJSON).
set -e
cd "$(dirname "$0")"
: "${ARDUINOJSON:=$HOME/Arduino/libraries/ArduinoJson/src}"
g++ -std=c++17 -Wall -Wno-unused-function -Ishim -I"$ARDUINOJSON" -I../money_bot \
  host_test.cpp ../money_bot/bot.cpp ../money_bot/store.cpp -o /tmp/money_bot_test
/tmp/money_bot_test
