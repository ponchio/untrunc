#include "log.h"

Logger::Level Logger::log_level = Logger::INFO;
Logger Log::error(Logger::ERROR);
Logger Log::info(Logger::INFO);
Logger Log::debug(Logger::DEBUG);
