#include <stdio.h>
#include <dirent.h>
#include <string.h>
#include <ctype.h>


int is_number(const char *str) {
    while (*str) {
        if (!isdigit(*str)) return 0;
        str++;
    }
    return 1;
}


int ps() {
    DIR *proc = opendir("/proc");
    if (!proc) {
        perror("Failed to open /proc");
        return 1;
    }
    struct dirent *entry;

    if (!proc) {
        perror("opendir /proc");
        return 1;
    }

    printf("%5s %s\n", "PID", "CMD");

    while ((entry = readdir(proc)) != NULL) {
        // Get the directory PIDs
        if (is_number(entry->d_name)) {
            char cmdline_path[256];
            FILE *cmdline_file;
            char cmdline[1024];

            snprintf(cmdline_path, sizeof(cmdline_path), "/proc/%s/cmdline", entry->d_name);
            cmdline_file = fopen(cmdline_path, "r");
            if (cmdline_file) {
                if (fgets(cmdline, sizeof(cmdline), cmdline_file)) {
                    // cmdline is null-separated replace with spaces
                    for (int i=0; i < strlen(cmdline); i++) {
                        if (cmdline[i] == '\0') cmdline[i] = ' ';
                    }
                    printf("%5s %s\n", entry->d_name, cmdline);
                }
                fclose(cmdline_file);
            }
        }
    }

    closedir(proc);
    return 0;
}