#pragma once
/* Tiny logger for hubnet: appends to the file set by net_set_log_file() and
 * mirrors to OutputDebugString. Thread safe. */

void net_set_log_file(const char* path);   // nullptr / "" disables the file
void net_log(const char* fmt, ...);
