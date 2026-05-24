#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <dirent.h>
#include <signal.h>
#include <sys/syslimits.h>

void csh_loop(void);
char* csh_read_line(void);
char** csh_split_line(const char* line);
void exec_redirect(char** args);
int csh_launch(char** args);
int csh_num_builtins();
int csh_cd(char** args);
int csh_help(char** args);
int csh_exit(char** args);
int csh_pwd(char** args);
int csh_history(char** args);
int csh_execute(char** args);
int csh_pipeline(char** args, int pipe_idx);

char* builtin_str[] = {
    "cd",
    "help",
    "history",
    "pwd",
    "exit",
};

int (*builtin_func[]) (char**) = {
    &csh_cd,
    &csh_help,
    &csh_history,
    &csh_pwd,
    &csh_exit
};

struct termios orig_termios;
int raw_mode_enabled = 0;

void disable_raw_mode() {
    if (raw_mode_enabled) {
        tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
        raw_mode_enabled = 0;
    }
}

void enable_raw_mode() {
    if (raw_mode_enabled) {
        return;
    }

    while (tcgetpgrp(STDIN_FILENO) != getpgrp()) {
        kill(0, SIGTTIN);
    }

    if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) {
        perror("csh: tcgetattr error");
        exit(EXIT_FAILURE);
    }

    atexit(disable_raw_mode);

    struct termios raw = orig_termios;
    raw.c_iflag &= ~(IXON | ICRNL);
    raw.c_oflag &= ~(OPOST);
    raw.c_lflag &= ~(ICANON | ECHO | IEXTEN | ISIG);

    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        perror("csh: tcsetattr error");
        exit(EXIT_FAILURE);
    }

    raw_mode_enabled = 1;
}

#define HIST_MAX 100

char* history[HIST_MAX];
int history_idx = 0;

void csh_add_history(const char* line) {
    if (line == NULL || strlen(line) == 0) return;
    if (history_idx > 0 && strcmp(history[history_idx - 1], line) == 0) return;
    if (history_idx < HIST_MAX) {
        history[history_idx++] = strdup(line);
    } else {
        free(history[0]);
        for (int i = 1; i < HIST_MAX; i++) {
            history[i - 1] = history[i];
        }
        history[HIST_MAX - 1] = strdup(line);
    }
}

int main(void) {
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);

    enable_raw_mode();
    csh_loop();
    return EXIT_SUCCESS;
}

void csh_loop(void) {
    int status;

    do {
        int bg_status;
        pid_t dead_bg_pid;
        while ((dead_bg_pid = waitpid(-1, &bg_status, WNOHANG)) > 0) {
            printf("\n\r[Background process %d completed]\n\r", dead_bg_pid);
        }

        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd)) != NULL) {
            printf("\033[1;32mcsh:%s\033[0m $ ", cwd);
        } else {
            printf("$ ");
        }
        fflush(stdout);

        char *line = csh_read_line();
        csh_add_history(line);
        char **args = csh_split_line(line);

        fflush(stdout);

        disable_raw_mode();
        status = csh_execute(args);
        enable_raw_mode();

        free(line);
        int j = 0;
        while (args[j] != NULL) {
            free(args[j]);
            j++;
        }
        free(args);
    } while (status);
}

#define CSH_RL_BUFSIZE 1024
char* csh_read_line(void) {
    int bufsize = CSH_RL_BUFSIZE;
    int position = 0;
    char* buffer = malloc(sizeof(char) * bufsize);

    if (!buffer) {
        fprintf(stderr, "csh: allocation error\n");
        exit(EXIT_FAILURE);
    }

    int history_index = history_idx;

    while (1) {
        unsigned char c;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n <= 0) continue;

        if (c == '\x1b') {
            char seq[2];
            if (read(STDIN_FILENO, &seq[0], 1) == 1 && read(STDIN_FILENO, &seq[1], 1) == 1) {
                if (seq[0] == '[') {
                    if (seq[1] == 'A') {
                        if (history_index > 0) {
                            history_index--;

                            while (position > 0) {
                                printf("\b \b");
                                position--;
                            }

                            strcpy(buffer, history[history_index]);
                            position = strlen(buffer);

                            printf("%s", buffer);
                            fflush(stdout);
                        }
                    }
                    else if (seq[1] == 'B') {
                        if (history_index < history_idx) {
                            history_index++;

                            while (position > 0) {
                                printf("\b \b");
                                position--;
                            }

                            if (history_index == history_idx) {
                                buffer[0] = '\0';
                                position = 0;
                            } else {
                                strcpy(buffer, history[history_index]);
                                position = strlen(buffer);
                                printf("%s", buffer);
                            }
                            fflush(stdout);
                        }
                    }
                }
            }
            continue;
        }

        if (c == '\n' || c == '\r') {
            buffer[position] = '\0';
            printf("\n\r");
            return buffer;
        } else if (c == 9) {
            if (position == 0) {
                printf("\a");
                fflush(stdout);
                continue;
            }
            int start = position;
            while (start > 0 && buffer[start - 1] != ' ') {
                start--;
            }

            int word_len = position - start;
            char* partial_word = malloc(sizeof(char) * (word_len + 1));
            strncpy(partial_word, &buffer[start], word_len);
            partial_word[word_len] = '\0';

            DIR *d = opendir(".");
            struct dirent *dir;
            int match_count = 0;

            if (d) {
                while ((dir = readdir(d)) != NULL) {
                    if (strncmp(dir->d_name, partial_word, word_len) == 0) {
                        match_count++;
                    }
                }
                closedir(d);
            }

            if (match_count == 1) {
                char* unique_match = NULL;
                d = opendir(".");
                if (d) {
                    while ((dir = readdir(d)) != NULL) {
                        if (strncmp(dir->d_name, partial_word, word_len) == 0) {
                            unique_match = dir->d_name;
                            break;
                        }
                    }

                    if (unique_match) {
                        int match_idx = word_len;
                        while (unique_match[match_idx] != '\0') {
                            char append_c = unique_match[match_idx];
                            buffer[position] = append_c;
                            position++;
                            printf("%c", append_c);
                            match_idx++;
                        }
                        buffer[position] = ' ';
                        position++;
                        printf(" ");
                    }
                    closedir(d);
                }
            } else if (match_count > 1) {
                printf("\n\r");
                d = opendir(".");
                if (d) {
                    while ((dir = readdir(d)) != NULL) {
                        if (strncmp(dir->d_name, partial_word, word_len) == 0) {
                            printf("%s ", dir->d_name);
                        }
                    }
                    closedir(d);
                }
                printf("\n\r");
                buffer[position] = '\0';
                printf("$ %s", buffer);
            } else {
                printf("\a");
            }

            fflush(stdout);
            free(partial_word);
            continue;
        } else if (c == 127 || c == 8) {
            if (position > 0) {
                position--;
                printf("\b \b");
                fflush(stdout);
            } else {
                printf("\a");
                fflush(stdout);
            }
            continue;
        } else {
            printf("%c", c);
            fflush(stdout);
            buffer[position] = c;
        }

        position++;

        if (position >= bufsize) {
            bufsize += CSH_RL_BUFSIZE;
            buffer = realloc(buffer, bufsize);
            if (!buffer) {
                fprintf(stderr, "csh: allocation error\n");
                exit(EXIT_FAILURE);
            }
        }
    }
}


#define CSH_TOK_BUFSIZE 64
char** csh_split_line(const char* line) {
    int bufsize = CSH_TOK_BUFSIZE;
    int position = 0;
    char** tokens = malloc(bufsize * sizeof(char*));

    int tok_bufsize = CSH_TOK_BUFSIZE;
    char* token = malloc(tok_bufsize * sizeof(char));
    int tok_pos = 0;

    int in_quote = 0;
    int i = 0;

    if (!tokens || !token) {
        fprintf(stderr, "csh: allocation error\n");
        exit(EXIT_FAILURE);
    }

    while (line[i] != '\0') {
        char c = line[i];

        if (c == '"') {
            in_quote = !in_quote;
        } else if ((c == ' ' || c == '\t' || c == '\r' || c == '\n') && !in_quote) {
            if (tok_pos > 0) {
                token[tok_pos] = '\0';
                tokens[position++] = strdup(token);
                tok_pos = 0;

                if (position >= bufsize) {
                    bufsize += CSH_TOK_BUFSIZE;
                    tokens = realloc(tokens, bufsize * sizeof(char*));
                    if (!tokens) {
                        fprintf(stderr, "csh: allocation error\n");
                        exit(EXIT_FAILURE);
                    }
                }
            }
        } else {
            if (tok_pos >= tok_bufsize - 1) {
                tok_bufsize += CSH_TOK_BUFSIZE;
                token = realloc(token, tok_bufsize * sizeof(char));
                if (!token) {
                    fprintf(stderr, "csh: allocation error\n");
                    exit(EXIT_FAILURE);
                }
            }

            token[tok_pos++] = c;
        }

        i++;
    }

    if (in_quote) {
        fprintf(stderr, "csh: syntax error: unterminated quote\n");
    }

    if (tok_pos > 0) {
        token[tok_pos] = '\0';
        tokens[position++] = strdup(token);

        if (position >= bufsize) {
            bufsize += CSH_TOK_BUFSIZE;
            tokens = realloc(tokens, bufsize * sizeof(char*));
            if (!tokens) {
                fprintf(stderr, "csh: allocation error\n");
                exit(EXIT_FAILURE);
            }
        }
    }

    tokens[position] = NULL;
    free(token);
    return tokens;
}

int csh_launch(char** args) {
    int status;
    int background = 0;

    int arg_count = 0;
    while (args[arg_count] != NULL) {
        arg_count++;
    }

    if (arg_count > 0 && strcmp(args[arg_count - 1], "&") == 0) {
        background = 1;
        args[arg_count - 1] = NULL;
    }

    pid_t pid = fork();
    if (pid == 0) {
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        signal(SIGTTIN, SIG_DFL);
        signal(SIGTTOU, SIG_DFL);
        setpgid(0, 0);
        if (!background) {
            tcsetpgrp(STDIN_FILENO, getpid());
        }
        exec_redirect(args);
        if (execvp(args[0], args) == -1) {
            perror("csh: execvp error");
        }
        exit(EXIT_FAILURE);
    }
    if (pid < 0) {
        perror("csh: fork error");
    } else {
        if (background) {
            printf("[Process running in background with PID %d]\n", pid);
        } else {
            setpgid(pid, pid);
            tcsetpgrp(STDIN_FILENO, pid);

            do {
                if (waitpid(pid, &status, WUNTRACED) == -1) {
                    perror("csh: waitpid error");
                    break;
                }
            } while (!WIFEXITED(status) && !WIFSIGNALED(status));

            tcsetpgrp(STDIN_FILENO, getpgrp());
        }
    }
    return 1;
}

void exec_redirect(char** args) {
    int i = 0;

    while (args[i] != NULL) {
        if (strcmp(args[i], ">") == 0) {
            if (args[i + 1] == NULL) {
                fprintf(stderr, "csh: syntax error near unexpected token 'newline'\n");
                exit(EXIT_FAILURE);
            }
            int fd = open(args[i + 1], O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) {
                perror("csh: open error");
                exit(EXIT_FAILURE);
            }

            if (dup2(fd, 1) < 0) {
                perror("csh: dup2 error");
                exit(EXIT_FAILURE);
            }
            close(fd);

            free(args[i]);
            free(args[i + 1]);

            int j = i;
            while (args[j + 2] != NULL) {
                args[j] = args[j + 2];
                j++;
            }
            args[j] = NULL;
            continue;
        }
        if (strcmp(args[i], "<") == 0) {
            if (args[i + 1] == NULL) {
                fprintf(stderr, "csh: syntax errpr near unexpected token 'newline'\n");
                exit(EXIT_FAILURE);
            }

            int fd = open(args[i + 1], O_RDONLY);
            if (fd < 0) {
                perror("csh: open error");
                exit(EXIT_FAILURE);
            }

            if (dup2(fd, 0) < 0) {
                perror("csh: dup2 error");
                exit(EXIT_FAILURE);
            }
            close(fd);

            free(args[i]);
            free(args[i + 1]);

            int j = i;
            while (args[j + 2] != NULL) {
                args[j] = args[j + 2];
                j++;
            }
            args[j] = NULL;
            continue;
        }
        i++;
    }
}

int csh_num_builtins() {
    return sizeof(builtin_str)/sizeof(char*) ;
}

int csh_cd(char** args) {
    if (args[1] == NULL) {
        fprintf(stderr, "csh: missing argument for 'cd'\n");
    } else {
        if (chdir(args[1]) != 0) {
            perror("csh: chdir error");
        }
    }
    return 1;
}

int csh_help(char** args) {
    (void)args;

    printf("Dmitriy Gagarin's CSH\n");
    printf("Type program names and arguments, and hit enter.\n");
    printf("The following are built in:\n");

    for (int i = 0; i < csh_num_builtins(); i++) {
        printf("%s\n", builtin_str[i]);
    }

    printf("Use the man command for information on other programs.\n");
    return 1;
}

int csh_exit(char** args) {
    (void)args;

    return 0;
}

int csh_pwd(char** args) {
    (void)args;

    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
        printf("%s\n", cwd);
    } else {
        perror("csh: getcwd error");
    }
    return 1;
}

int csh_history(char** args) {
    (void)args;

    for (int i = 0; i < history_idx; i++) {
        printf("%d. %s\n", i + 1, history[i]);
    }
    return 1;
}

int csh_execute(char** args) {
    if (args[0] == NULL) {
        return 1;
    }

    int i = 0;
    while (args[i] != NULL) {
        if (strcmp(args[i], "|") == 0) {
            return csh_pipeline(args, i);
        }
        i++;
    }

    for (int i = 0; i < csh_num_builtins(); i++) {
        if (strcmp(args[0], builtin_str[i]) == 0) {
            return (*builtin_func[i])(args);
        }
    }
    return csh_launch(args);
}

int csh_pipeline(char** args, int pipe_idx) {
    args[pipe_idx] = NULL;
    char** left_args = args;
    char** right_args = &args[pipe_idx + 1];

    if (left_args[0] == NULL || right_args[0] == NULL) {
        fprintf(stderr, "csh: syntax error near unexpected token '|'\n");
        return 1;
    }

    int pipefd[2];

    if (pipe(pipefd) < 0) {
        perror("csh: pipe error");
        return 1;
    }

    pid_t pid1 = fork();
    if (pid1 == 0) {
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        signal(SIGTTIN, SIG_DFL);
        signal(SIGTTOU, SIG_DFL);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);

        exec_redirect(left_args);
        if (execvp(left_args[0], left_args) == -1) {
            perror("csh: execvp error");
        }
        exit(EXIT_FAILURE);
    }

    if (pid1 < 0) {
        perror("csh: fork error");
        close(pipefd[0]);
        close(pipefd[1]);
        return 1;
    }

    pid_t pid2 = fork();
    if (pid2 == 0) {
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        signal(SIGTTIN, SIG_DFL);
        signal(SIGTTOU, SIG_DFL);
        dup2(pipefd[0], STDIN_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);

        exec_redirect(right_args);
        if (execvp(right_args[0], right_args) == -1) {
            perror("csh: execvp error");
        }
        exit(EXIT_FAILURE);
    }

    if (pid2 < 0) {
        perror("csh: fork error");
        close(pipefd[0]);
        close(pipefd[1]);
        waitpid(pid1, NULL, 0);
        return 1;
    }

    close(pipefd[0]);
    close(pipefd[1]);

    int status;
    waitpid(pid1, &status, 0);
    waitpid(pid2, &status, 0);

    return 1;
}
