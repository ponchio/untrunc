#ifndef LOG_H
#define LOG_H

#include <iostream>
#include <string>

class Logger {
public:
	enum Level { SILENT = 0, ERROR = 1, INFO = 3, DEBUG_1 = 4 };
	Level level;
	static Level log_level;

	Logger(Level _level): level(_level) {}

	template<class T> Logger &operator<<(const T &msg) {
		if(level <= Logger::log_level)
			std::cout << msg;
		return *this;
	}

	// define an operator<< to take in std::endl
	typedef std::basic_ostream<char, std::char_traits<char> > CoutType;
	typedef CoutType& (*StandardEndLine)(CoutType&);
	Logger& operator<<(StandardEndLine manip) {
		if(level <= Logger::log_level)
			std::cout << std::endl;
		return *this;
	}


};

class Log {
public:
	static Logger error;
	static Logger info;
	static Logger debug;
	static void flush() { std::cout << std::flush; }
};

#endif // LOG_H

