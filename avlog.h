#ifndef AVLOG_H
#define AVLOG_H

#ifndef __STDC_LIMIT_MACROS
# define __STDC_LIMIT_MACROS    1
#endif
#ifndef __STDC_CONSTANT_MACROS
# define __STDC_CONSTANT_MACROS 1
#endif
extern "C" {
#include <stdint.h>
}
#ifndef INT64_C
# define INT64_C(c)     (c ## LL)
# define UINT64_C(c)    (c ## ULL)
#endif

// Prevent #define of bool, true & false.
#undef  __bool_true_false_are_defined
#define __bool_true_false_are_defined   1
#ifndef _Bool
# define _Bool      bool
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
//#include <libavutil/log.h>

# include <config.h>

// XXX Horrible Hack: Suppress C99 keywords that are not in C++, like 'restrict' and '_Atomic'! XXX
#undef  restrict    // Harmless; don't restrict memory access.
#define restrict
#undef  _Atomic     // Atomics are only included in headers, but never actually used in our code.
#define _Atomic
// XXX Horrible Hack: There are variables named 'new' and 'class' inside! XXX
#define new         extern_new
#define class       extern_class
#include <libavcodec/h264dec.h>
#undef class
#undef new
#undef _Atomic
#undef restrict
}

namespace {


	// Configure FFmpeg/Libav logging for use in C++.
	class AvLog {
		int lvl;
#ifdef AV_LOG_PRINT_LEVEL
		int flgs;
#endif
	public:
#ifdef AV_LOG_PRINT_LEVEL
# define DEFAULT_AVLOG_FLAGS	AV_LOG_PRINT_LEVEL
#else
# define DEFAULT_AVLOG_FLAGS	0
#endif

		explicit AvLog()
			: lvl(av_log_get_level())
#ifdef AV_LOG_PRINT_LEVEL
			, flgs(av_log_get_flags())
#endif
		{
			av_log_set_flags(DEFAULT_AVLOG_FLAGS);
			std::cout.flush();   // Flush C++ standard streams.
			std::cerr.flush();   // Default unbuffered -> Flush in case this has changed.
			std::clog.flush();
		}

		explicit AvLog(int level, int flags = DEFAULT_AVLOG_FLAGS)
			: lvl(av_log_get_level())
#ifdef AV_LOG_PRINT_LEVEL
			, flgs(av_log_get_flags())
#endif
		{
			if(lvl < level)
				av_log_set_level(lvl);
			av_log_set_flags(flags);
			std::cout.flush();   // Flush C++ standard streams.
			std::cerr.flush();   // Default unbuffered -> Flush in case this has changed.
			std::clog.flush();
		}

		~AvLog() {
			fflush(stdout); // Flush C stdio files.
			fflush(stderr);
			av_log_set_level(lvl);
#ifdef AV_LOG_PRINT_LEVEL
			av_log_set_flags(flgs);
#endif
		}
	};
}; // namespace

#endif // AVLOG_H

