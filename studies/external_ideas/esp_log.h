/**
 * Macro per convertire le chiamate ESP_LOG* in printf con colori ANSI
 * Da includere nel proprio progetto per sostituire le funzioni ESP_LOG originali
 */

 #ifndef ESP_LOG_COMPAT_H
 #define ESP_LOG_COMPAT_H
 
 #include <stdio.h>
 #include <time.h>
 
 /* Codici colore ANSI */
 #define LOG_COLOR_BLACK   "\033[0;30m"
 #define LOG_COLOR_RED     "\033[0;31m"
 #define LOG_COLOR_GREEN   "\033[0;32m"
 #define LOG_COLOR_YELLOW  "\033[0;33m"
 #define LOG_COLOR_BLUE    "\033[0;34m"
 #define LOG_COLOR_MAGENTA "\033[0;35m"
 #define LOG_COLOR_CYAN    "\033[0;36m"
 #define LOG_COLOR_WHITE   "\033[0;37m"
 #define LOG_COLOR_RESET   "\033[0m"
 
 /* Livelli di log */
 #define LOG_LEVEL_INFO    "I"
 #define LOG_LEVEL_WARNING "W"
 #define LOG_LEVEL_ERROR   "E"
 
 /* Ottieni timestamp corrente */
 #define GET_TIMESTAMP() ({ \
     char _timestamp[16]; \
     time_t _now = time(NULL); \
     struct tm _tm = *localtime(&_now); \
     strftime(_timestamp, sizeof(_timestamp), "%H:%M:%S", &_tm); \
     _timestamp; \
 })
 
 /* Macro principale per il logging */
 #define LOG_PRINTF(color, level, tag, format, ...) \
     printf(color "[%s] %s (%s): " format LOG_COLOR_RESET "\n", \
            GET_TIMESTAMP(), level, tag, ##__VA_ARGS__)
 
 /* Sostituzioni per le funzioni ESP_LOG originali */
 #define ESP_LOGI(tag, format, ...) \
     LOG_PRINTF(LOG_COLOR_GREEN, LOG_LEVEL_INFO, tag, format, ##__VA_ARGS__)
 
 #define ESP_LOGW(tag, format, ...) \
     LOG_PRINTF(LOG_COLOR_YELLOW, LOG_LEVEL_WARNING, tag, format, ##__VA_ARGS__)
 
 #define ESP_LOGE(tag, format, ...) \
     LOG_PRINTF(LOG_COLOR_RED, LOG_LEVEL_ERROR, tag, format, ##__VA_ARGS__)
 
 #endif /* ESP_LOG_COMPAT_H */