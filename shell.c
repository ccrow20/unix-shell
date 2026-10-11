#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_INPUT 4096
#define MAX_ARGS 256
#define MAX_COMMANDS 64

struct command {
    char *argv[MAX_ARGS];
    char *input_file;
    char *output_file;
};

struct child_args {
    struct command *cmd;
    char *path;
    int input_fd;
    int output_fd;
};

static char lt_token[] = "<";
static char gt_token[] = ">";
static char pipe_token[] = "|";
static char child_stack[64 * 1024] __attribute__((aligned(16)));

static void sigint_handler(int sig)
{
    (void)sig;
}

static void install_shell_signals(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    signal(SIGQUIT, SIG_IGN);
}

static void reset_child_signals(void)
{
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
}

static char *operator_token(char c)
{
    if (c == '<')
        return lt_token;
    if (c == '>')
        return gt_token;
    return pipe_token;
}

static int is_operator(char c)
{
    return c == '<' || c == '>' || c == '|';
}

static int tokenize(char *input, char *tokens[])
{
    int count = 0;
    char *p = input;

    while (*p != '\0' && count < MAX_ARGS - 1) {
        while (isspace((unsigned char)*p))
            p++;

        if (*p == '\0')
            break;

        if (is_operator(*p)) {
            tokens[count++] = operator_token(*p);
            p++;
            continue;
        }

        tokens[count++] = p;

        while (*p != '\0' && !isspace((unsigned char)*p) && !is_operator(*p))
            p++;

        if (*p == '\0')
            break;

        if (is_operator(*p)) {
            char special = *p;

            *p = '\0';
            p++;
            if (count < MAX_ARGS - 1)
                tokens[count++] = operator_token(special);
        } else {
            *p = '\0';
            p++;
        }
    }

    tokens[count] = NULL;
    return count;
}

static int parse_commands(char *tokens[], int ntokens, struct command commands[])
{
    int command_count = 1;
    int argc = 0;

    memset(commands, 0, sizeof(struct command) * MAX_COMMANDS);

    for (int i = 0; i < ntokens; i++) {
        char *token = tokens[i];

        if (strcmp(token, "|") == 0) {
            if (command_count >= MAX_COMMANDS)
                return -1;
            command_count++;
            argc = 0;
        } else if (strcmp(token, "<") == 0) {
            if (i + 1 >= ntokens || is_operator(tokens[i + 1][0]))
                return -1;
            commands[command_count - 1].input_file = tokens[++i];
        } else if (strcmp(token, ">") == 0) {
            if (i + 1 >= ntokens || is_operator(tokens[i + 1][0]))
                return -1;
            commands[command_count - 1].output_file = tokens[++i];
        } else {
            if (argc >= MAX_ARGS - 1)
                return -1;
            commands[command_count - 1].argv[argc++] = token;
            commands[command_count - 1].argv[argc] = NULL;
        }
    }

    for (int i = 0; i < command_count; i++) {
        if (commands[i].argv[0] == NULL)
            return -1;
    }

    return command_count;
}

static char *find_in_path(char *program, char *path_buffer, size_t path_buffer_size)
{
    if (strchr(program, '/') != NULL) {
        snprintf(path_buffer, path_buffer_size, "%s", program);
        return path_buffer;
    }

    char *path = getenv("PATH");

    if (path == NULL)
        return NULL;

    char *copy = strdup(path);

    if (copy == NULL)
        return NULL;

    char *saveptr = NULL;
    char *directory = strtok_r(copy, ":", &saveptr);

    while (directory != NULL) {
        snprintf(path_buffer, path_buffer_size, "%s/%s", directory, program);

        if (access(path_buffer, X_OK) == 0) {
            free(copy);
            return path_buffer;
        }

        directory = strtok_r(NULL, ":", &saveptr);
    }

    free(copy);
    return NULL;
}

static void child_exec(struct command *cmd, char *path, int input_fd, int output_fd)
{
    reset_child_signals();

    if (input_fd != STDIN_FILENO && cmd->input_file == NULL) {
        if (dup3(input_fd, STDIN_FILENO, 0) == -1)
            _exit(1);
    }

    if (output_fd != STDOUT_FILENO && cmd->output_file == NULL) {
        if (dup3(output_fd, STDOUT_FILENO, 0) == -1)
            _exit(1);
    }

    if (input_fd != STDIN_FILENO)
        close(input_fd);

    if (output_fd != STDOUT_FILENO)
        close(output_fd);

    if (cmd->input_file != NULL) {
        int fd = open(cmd->input_file, O_RDONLY);

        if (fd == -1) {
            dprintf(STDERR_FILENO, "open %s: %s\n", cmd->input_file, strerror(errno));
            _exit(1);
        }

        if (dup3(fd, STDIN_FILENO, 0) == -1)
            _exit(1);

        close(fd);
    }

    if (cmd->output_file != NULL) {
        int fd = open(cmd->output_file, O_WRONLY | O_CREAT | O_TRUNC, 0666);

        if (fd == -1) {
            dprintf(STDERR_FILENO, "open %s: %s\n", cmd->output_file, strerror(errno));
            _exit(1);
        }

        if (dup3(fd, STDOUT_FILENO, 0) == -1)
            _exit(1);

        close(fd);
    }

    if (path == NULL) {
        dprintf(STDERR_FILENO, "Unrecognized command: %s\n", cmd->argv[0]);
        _exit(127);
    }

    execve(path, cmd->argv, environ);
    dprintf(STDERR_FILENO, "%s: %s\n", cmd->argv[0], strerror(errno));
    _exit(127);
}

static int child_main(void *arg)
{
    struct child_args *a = arg;

    child_exec(a->cmd, a->path, a->input_fd, a->output_fd);
    return 127;
}

static pid_t create_child(struct command *cmd, int input_fd, int output_fd)
{
    char path_buffer[MAX_INPUT];
    char *path = find_in_path(cmd->argv[0], path_buffer, sizeof(path_buffer));

    struct child_args args = { cmd, path, input_fd, output_fd };
    pid_t pid = clone(child_main, child_stack + sizeof(child_stack),
                      CLONE_VM | CLONE_VFORK | SIGCHLD, &args);

    if (pid == -1)
        perror("clone");

    return pid;
}

static void print_prompt(void)
{
    char cwd[MAX_INPUT];

    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        perror("getcwd");
        return;
    }

    printf("%s$ ", cwd);
    fflush(stdout);
}

int main(void)
{
    char input[MAX_INPUT];

    install_shell_signals();

    while (1) {
        print_prompt();

        errno = 0;
        if (fgets(input, sizeof(input), stdin) == NULL) {
            if (errno == EINTR) {
                clearerr(stdin);
                printf("\n");
                continue;
            }
            break;
        }

        input[strcspn(input, "\n")] = '\0';

        char *tokens[MAX_ARGS];
        int ntokens = tokenize(input, tokens);

        if (ntokens == 0)
            continue;

        struct command commands[MAX_COMMANDS];
        int command_count = parse_commands(tokens, ntokens, commands);

        if (command_count < 0) {
            fprintf(stderr, "Invalid command\n");
            continue;
        }

        if (strcmp(commands[0].argv[0], "exit") == 0) {
            if (commands[0].argv[1] != NULL) {
                fprintf(stderr, "exit: too many arguments\n");
                continue;
            }
            return 0;
        }

        if (strcmp(commands[0].argv[0], "cd") == 0) {
            if (commands[0].argv[1] == NULL || commands[0].argv[2] != NULL) {
                fprintf(stderr, "usage: cd PATH\n");
                continue;
            }

            if (chdir(commands[0].argv[1]) == -1)
                fprintf(stderr, "cd: %s: %s\n", commands[0].argv[1], strerror(errno));

            continue;
        }

        if (strcmp(commands[0].argv[0], "exec") == 0) {
            if (commands[0].argv[1] == NULL) {
                fprintf(stderr, "usage: exec PROGRAM [ARGUMENTS...]\n");
                continue;
            }

            char path_buffer[MAX_INPUT];
            char *path = find_in_path(commands[0].argv[1], path_buffer, sizeof(path_buffer));

            if (path == NULL) {
                fprintf(stderr, "exec: %s: command not found\n", commands[0].argv[1]);
                continue;
            }

            reset_child_signals();
            execve(path, &commands[0].argv[1], environ);
            fprintf(stderr, "exec: %s\n", strerror(errno));
            install_shell_signals();
            continue;
        }

        int previous_read = STDIN_FILENO;
        pid_t pids[MAX_COMMANDS] = {0};

        for (int i = 0; i < command_count; i++) {
            int pipe_fds[2] = {-1, -1};

            if (i < command_count - 1) {
                if (pipe2(pipe_fds, 0) == -1) {
                    perror("pipe2");
                    break;
                }
            }

            int output_fd = (i < command_count - 1) ? pipe_fds[1] : STDOUT_FILENO;

            pids[i] = create_child(&commands[i], previous_read, output_fd);

            if (previous_read != STDIN_FILENO)
                close(previous_read);

            if (pipe_fds[1] != -1)
                close(pipe_fds[1]);

            previous_read = STDIN_FILENO;

            if (pids[i] == -1) {
                if (pipe_fds[0] != -1)
                    close(pipe_fds[0]);
                break;
            }

            if (pipe_fds[0] != -1)
                previous_read = pipe_fds[0];
        }

        if (previous_read != STDIN_FILENO)
            close(previous_read);

        int interrupted = 0;

        for (int i = 0; i < command_count; i++) {
            if (pids[i] <= 0)
                continue;

            int status = 0;

            while (waitpid(pids[i], &status, 0) == -1 && errno == EINTR)
                ;

            if (WIFSIGNALED(status) && WTERMSIG(status) == SIGINT)
                interrupted = 1;
        }

        if (interrupted)
            printf("\n");
    }

    return 0;
}
