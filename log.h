#ifndef LOG_H
#define LOG_H

/* Logging shared by every module: UTC-timestamped lines on stdout. */
enum {
    LEVEL_ALWAYS = -1,
    LEVEL_ERROR = 0,
    LEVEL_INFO,
    LEVEL_DEBUG,
    LEVEL_TRACE
};

extern int log_level;
void log_print(int level, const char *level_name, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#define LOG_ALWAYS(fmt, ...) log_print(LEVEL_ALWAYS, "ALWAYS", fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) log_print(LEVEL_ERROR, "ERROR", fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  log_print(LEVEL_INFO,  "INFO",  fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) log_print(LEVEL_DEBUG, "DEBUG", fmt, ##__VA_ARGS__)
#define LOG_TRACE(fmt, ...) log_print(LEVEL_TRACE, "TRACE", fmt, ##__VA_ARGS__)

#endif
