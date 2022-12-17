#ifdef READLINE
#include <readline/readline.h>
#include <readline/history.h>
#endif

#define DEBUG 0
#include "shell.h"

sigset_t sigchld_mask;

static void sigint_handler(int sig) {
  /* No-op handler, we just need break read() call with EINTR. */
  (void)sig;
}

/* Rewrite closed file descriptors to -1,
 * to make sure we don't attempt do close them twice. */
static void MaybeClose(int *fdp) {
  if (*fdp < 0)
    return;
  Close(*fdp);
  *fdp = -1;
}

/* Consume all tokens related to redirection operators.
 * Put opened file descriptors into inputp & output respectively. */
static int do_redir(token_t *token, int ntokens, int *inputp, int *outputp) {
  token_t mode = NULL; /* T_INPUT, T_OUTPUT or NULL */
  int n = 0;           /* number of tokens after redirections are removed */

  for (int i = 0; i < ntokens; i++) {
    /* TODO: Handle tokens and open files as requested. */
#ifdef STUDENT
    if (token[i] == T_INPUT) {
      mode = T_INPUT;
      continue;
    } else if (token[i] == T_OUTPUT) {
      mode = T_OUTPUT;
      continue;
    }
    if (mode != NULL) {
      int fd = -1;
      if (mode == T_INPUT) {
        fd = open(token[i], O_RDONLY);
        if (fd < 0)
          app_error("Failed to open a file: file %s, line %d", __FILE__,
                    __LINE__);

        *inputp = fd; // 0 zostanie wczesniej zamkniety, bo tak dziala dup2
      } else if (mode == T_OUTPUT) {
        fd = open(token[i], O_WRONLY | O_CREAT | O_TRUNC,
                  S_IRUSR | S_IWUSR | S_IRGRP |
                    S_IROTH); // takie bity ustawiaja plikowi zsh i bash u mnie
                              // (to be changed?)
        if (fd < 0)
          app_error("Failed to open a file: file %s, line %d", __FILE__,
                    __LINE__);

        *outputp = fd;
      }

    } else
      ++n;

#endif /* !STUDENT */
  }

  token[n] = NULL;
  return n;
}

/* Execute internal command within shell's process or execute external command
 * in a subprocess. External command can be run in the background. */
static int do_job(token_t *token, int ntokens, bool bg) {
  int input = -1, output = -1;
  int exitcode = 0;

  ntokens = do_redir(token, ntokens, &input, &output);

  if (!bg) {
    if ((exitcode = builtin_command(token)) >= 0)
      return exitcode;
  }

  sigset_t mask;
  Sigprocmask(SIG_BLOCK, &sigchld_mask, &mask);

  /* TODO: Start a subprocess, create a job and monitor it. */
#ifdef STUDENT  
 // fprintf(stderr,"%ld???\n",(long)mask);
  int pid = fork();
  if (pid < 0)
    app_error("fork failed: file %s, line %d", __FILE__, __LINE__);
  else if (pid > 0) {
    MaybeClose(&input);
    MaybeClose(&output);
    int j = addjob(pid, bg);
    addproc(j, pid, token);
    if (!bg) {
      
      exitcode = monitorjob(&mask);
    }
    else{
      char *cmd;
      int chars=0;
      for(int i=0;i<ntokens;++i) chars+=strlen(token[i])+1;
      cmd=malloc(chars);
      int offset=0;
      for (int i=0;i<ntokens;){
        int len=strlen(token[i]);
        strcpy(cmd+offset,token[i++]);
        if(i!=ntokens){
          cmd[offset+=len]=' ';
          ++offset;
        }
      }
      msg("[%d] running '%s'\n", j,cmd);
      free(cmd);
    }

  } else {
    signal(SIGINT,SIG_DFL);
    signal (SIGQUIT, SIG_DFL);
    signal(SIGTSTP,SIG_DFL);
    signal(SIGTTIN,SIG_DFL);
    signal(SIGTTOU,SIG_DFL);
    signal (SIGCHLD, SIG_DFL);

    setpgid(0, 0);
    if (input != -1) {
      dup2(input, STDIN_FILENO);
      close(input);
    }
    if (output != -1) {
      dup2(output, STDOUT_FILENO);
      close(output);
    }
    external_command(token);
    app_error("execve failed: file %s, line %d", __FILE__, __LINE__);
  }
#endif /* !STUDENT */

  Sigprocmask(SIG_SETMASK, &mask, NULL);
  return exitcode;
}

/* Start internal or external command in a subprocess that belongs to pipeline.
 * All subprocesses in pipeline must belong to the same process group. */
static pid_t do_stage(pid_t pgid, sigset_t *mask, int input, int output,
                      token_t *token, int ntokens, bool bg) {
  ntokens = do_redir(token, ntokens, &input, &output);

  if (ntokens == 0)
    app_error("ERROR: Command line is not well formed!");

  /* TODO: Start a subprocess and make sure it's moved to a process group. */
  pid_t pid = Fork();
#ifdef STUDENT
  if (pid < 0)
    app_error("fork failed: file %s, line %d", __FILE__, __LINE__);
  else if (pid == 0) {
    signal(SIGINT,SIG_DFL);
    signal (SIGQUIT, SIG_DFL);
    signal(SIGTSTP,SIG_DFL);
    signal(SIGTTIN,SIG_DFL);
    signal(SIGTTOU,SIG_DFL);
    signal (SIGCHLD, SIG_DFL);

    setpgid(0, pgid ? pgid : getpid());

    if (input != STDIN_FILENO) {
      dup2(input, STDIN_FILENO);
      close(input);
    }
    if (output != STDOUT_FILENO) {
      dup2(output, STDOUT_FILENO);
      close(output);
    }

    if (!bg) {
      int exitcode = 0;
      if ((exitcode = builtin_command(token)) >= 0)
        exit(exitcode);
    }

    external_command(token);
    app_error("execve failed: file %s, line %d", __FILE__, __LINE__);
  }
#endif /* !STUDENT */

  return pid;
}

static void mkpipe(int *readp, int *writep) {
  int fds[2];
  Pipe(fds);
  fcntl(fds[0], F_SETFD, FD_CLOEXEC);
  fcntl(fds[1], F_SETFD, FD_CLOEXEC);
  *readp = fds[0];
  *writep = fds[1];
}

/* Pipeline execution creates a multiprocess job. Both internal and external
 * commands are executed in subprocesses. */
static int do_pipeline(token_t *token, int ntokens, bool bg) {
  pid_t pid, pgid = 0;
  int job = -1;
  int exitcode = 0;

  int input = -1, output = -1, next_input = -1;

  mkpipe(&next_input, &output);

  sigset_t mask;
  Sigprocmask(SIG_BLOCK, &sigchld_mask, &mask);

  /* TODO: Start pipeline subprocesses, create a job and monitor it.
   * Remember to close unused pipe ends! */
#ifdef STUDENT
  int j = 0;
  while (token[j] && token[j] != T_PIPE)
    ++j; // wyznaczamy tokeny do wykonania kolejnego procesu w pipelinie
  token_t *args = malloc((j + 1) * sizeof(token_t));
  for (int i2 = 0; i2 < j; ++i2)
    args[i2] = token[i2]; // nie mozna niestety uzyc strncpy
  args[j] = NULL;

  pid = do_stage(0, &mask, STDIN_FILENO, output, args, j, bg);
  pgid = pid;

  job = addjob(pgid, bg);
  addproc(job, pid, args);
  free(args);

  ++j;
  for (int i = j; i < ntokens; i = ++j) {
    while (token[j] && token[j] != T_PIPE)
      ++j; // wyznaczamy tokeny do wykonania kolejnego procesu w pipelinie
    args = malloc((j - i + 1) * sizeof(token_t));
    for (int i2 = 0; i2 < j - i; ++i2)
      args[i2] = token[i + i2];
    args[j - i] = NULL;

    MaybeClose(
      &input); // ustawiamy deskryptory tak, by był pipe między kolejnymi
               // procesami (stdin i stdout ogarniamy w do_stage)
    input = dup(next_input);
    MaybeClose(&next_input);
    MaybeClose(&output);

    if (j == ntokens)
      pid = do_stage(pgid, &mask, input, STDOUT_FILENO, args, j - i, bg);
    else {
      mkpipe(&next_input, &output);
      pid = do_stage(pgid, &mask, input, output, args, j - i, bg);
    }
    addproc(job, pid, args);
    free(args);
  }
  MaybeClose(&next_input);
  MaybeClose(&output);
  MaybeClose(&input);

  if (!bg) {
    exitcode = monitorjob(&mask);
  }
  // else msg("[%d] running '%s'\n", j,jobs[i].command);
  

  (void)input;
  (void)job;
  (void)pid;
  (void)pgid;
  (void)do_stage;

#endif /* !STUDENT */

  Sigprocmask(SIG_SETMASK, &mask, NULL);
  return exitcode;
}

static bool is_pipeline(token_t *token, int ntokens) {
  for (int i = 0; i < ntokens; i++)
    if (token[i] == T_PIPE)
      return true;
  return false;
}

static void eval(char *cmdline) {
  bool bg = false;
  int ntokens;
  token_t *token = tokenize(cmdline, &ntokens);

  if (ntokens > 0 && token[ntokens - 1] == T_BGJOB) {
    token[--ntokens] = NULL;
    bg = true;
  }

  if (ntokens > 0) {
    if (is_pipeline(token, ntokens)) {
      do_pipeline(token, ntokens, bg);
    } else {
      do_job(token, ntokens, bg);
    }
  }

  free(token);
}

#ifndef READLINE
static char *readline(const char *prompt) {
  static char line[MAXLINE]; /* `readline` is clearly not reentrant! */

  write(STDOUT_FILENO, prompt, strlen(prompt));

  line[0] = '\0';

  ssize_t nread = read(STDIN_FILENO, line, MAXLINE);
  if (nread < 0) {
    if (errno != EINTR)
      unix_error("Read error");
    msg("\n");
  } else if (nread == 0) {
    return NULL; /* EOF */
  } else {
    if (line[nread - 1] == '\n')
      line[nread - 1] = '\0';
  }

  return strdup(line);
}
#endif

int main(int argc, char *argv[]) {
  /* `stdin` should be attached to terminal running in canonical mode */
  if (!isatty(STDIN_FILENO))
    app_error("ERROR: Shell can run only in interactive mode!");

#ifdef READLINE
  rl_initialize();
#endif

  sigemptyset(&sigchld_mask);
  sigaddset(&sigchld_mask, SIGCHLD);

  if (getsid(0) != getpgid(0))
    Setpgid(0, 0);

  initjobs();

  struct sigaction act = {
    .sa_handler = sigint_handler,
    .sa_flags = 0, /* without SA_RESTART read() will return EINTR */
  };
  Sigaction(SIGINT, &act, NULL);

  Signal(SIGTSTP, SIG_IGN);
  Signal(SIGTTIN, SIG_IGN);
  Signal(SIGTTOU, SIG_IGN);

  while (true) {
    char *line = readline("# ");

    if (line == NULL)
      break;

    if (strlen(line)) {
#ifdef READLINE
      add_history(line);
#endif
      eval(line);
    }
    free(line);
    watchjobs(FINISHED);
  }

  msg("\n");
  shutdownjobs();

  return 0;
}
