#include "shell.h"

typedef struct proc {
  pid_t pid;    /* process identifier */
  int state;    /* RUNNING or STOPPED or FINISHED */
  int exitcode; /* -1 if exit status not yet received */
} proc_t;

typedef struct job {
  pid_t pgid;            /* 0 if slot is free */
  proc_t *proc;          /* array of processes running in as a job */
  struct termios tmodes; /* saved terminal modes */
  int nproc;             /* number of processes */
  int state;             /* changes when live processes have same state */
  char *command;         /* textual representation of command line */
} job_t;

static job_t *jobs = NULL;          /* array of all jobs */
static int njobmax = 1;             /* number of slots in jobs array */
static int tty_fd = -1;             /* controlling terminal file descriptor */
static struct termios shell_tmodes; /* saved shell terminal modes */

static void sigchld_handler(int sig) {
  int old_errno = errno;
  pid_t pid;
  int status;
  /* TODO: Change state (FINISHED, RUNNING, STOPPED) of processes and jobs.
   * Bury all children that finished saving their status in jobs. */
#ifdef STUDENT
  (void)status;
  (void)pid;
  // pid = waitpid(-1, &status, WNOHANG | WUNTRACED);

  do {
    pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED);
    int state = RUNNING;
    if (WIFEXITED(status) || WIFSIGNALED(status))
      state = FINISHED;
    else if (WIFSTOPPED(status))
      state = STOPPED;
    else if (WIFCONTINUED(status))
      state = RUNNING;
    for (int i = 0; i < njobmax; ++i) {
      bool found = false;
      bool all = true; // czy wszystkie procesy w jobie maja taki sam stan
      for (int j = 0; j < jobs[i].nproc; ++j) {
        if (jobs[i].proc[j].pid == pid) {
          found = true;
          jobs[i].proc[j].state = state;
          if (state == FINISHED) {
            jobs[i].proc[j].exitcode = status;

          } else if (state == STOPPED) {
            tcgetattr(tty_fd, &jobs[i].tmodes);
          }
        } else if (jobs[i].proc[j].state != state)
          all = false;
      }

      if (found) {
        if (all)
          jobs[i].state = state; // job zmienia stan, gdy wszystkie jego procesy
                                 // maja nowy stan
        break;
      }
    }
  } while (pid > 0);

#endif /* !STUDENT */
  errno = old_errno;
}

/* When pipeline is done, its exitcode is fetched from the last process. */
static int exitcode(job_t *job) {
  return job->proc[job->nproc - 1].exitcode;
}

static int allocjob(void) {
  /* Find empty slot for background job. */
  for (int j = BG; j < njobmax; j++)
    if (jobs[j].pgid == 0)
      return j;

  /* If none found, allocate new one. */
  jobs = realloc(jobs, sizeof(job_t) * (njobmax + 1));
  memset(&jobs[njobmax], 0, sizeof(job_t));
  return njobmax++;
}

static int allocproc(int j) {
  job_t *job = &jobs[j];
  job->proc = realloc(job->proc, sizeof(proc_t) * (job->nproc + 1));
  return job->nproc++;
}

int addjob(pid_t pgid, int bg) {
  int j = bg ? allocjob() : FG;
  job_t *job = &jobs[j];
  /* Initial state of a job. */
  job->pgid = pgid;
  job->state = RUNNING;
  job->command = NULL;
  job->proc = NULL;
  job->nproc = 0;
  job->tmodes = shell_tmodes;
  return j;
}

static void deljob(job_t *job) {
  assert(job->state == FINISHED);
  free(job->command);
  free(job->proc);
  job->pgid = 0;
  job->command = NULL;
  job->proc = NULL;
  job->nproc = 0;
}

static void movejob(int from, int to) {
  assert(jobs[to].pgid == 0);
  memcpy(&jobs[to], &jobs[from], sizeof(job_t));
  memset(&jobs[from], 0, sizeof(job_t));
}

static void mkcommand(char **cmdp, char **argv) {
  if (*cmdp)
    strapp(cmdp, " | ");

  for (strapp(cmdp, *argv++); *argv; argv++) {
    strapp(cmdp, " ");
    strapp(cmdp, *argv);
  }
}

void addproc(int j, pid_t pid, char **argv) {
  assert(j < njobmax);
  job_t *job = &jobs[j];

  int p = allocproc(j);
  proc_t *proc = &job->proc[p];
  /* Initial state of a process. */
  proc->pid = pid;
  proc->state = RUNNING;
  proc->exitcode = -1;
  mkcommand(&job->command, argv);
}

/* Returns job's state.
 * If it's finished, delete it and return exitcode through statusp. */
static int jobstate(int j, int *statusp) {
  assert(j < njobmax);
  job_t *job = &jobs[j];
  int state = job->state;

  /* TODO: Handle case where job has finished. */
#ifdef STUDENT
  // (void)exitcode;
  if (state == FINISHED &&
      statusp) // jak chcemy sam state, to można dać statusp=NULL
    *statusp = exitcode(job);
#endif /* !STUDENT */

  return state;
}

char *jobcmd(int j) {
  assert(j < njobmax);
  job_t *job = &jobs[j];
  return job->command;
}

/* Continues a job that has been stopped. If move to foreground was requested,
 * then move the job to foreground and start monitoring it. */
bool resumejob(int j, int bg, sigset_t *mask) {
  if (j < 0) {
    for (j = njobmax - 1; j > 0 && jobs[j].state == FINISHED; j--)
      continue;
  }

  if (j >= njobmax || jobs[j].state == FINISHED)
    return false;

    /* TODO: Continue stopped job. Possibly move job to foreground slot. */
#ifdef STUDENT
  msg("[%d] continue '%s'\n", j, jobs[j].command);
  if (!bg) {
    deljob(&jobs[0]);
    movejob(j, 0);
    j = 0;
    setfgpgrp(jobs[j].pgid);
    tcsetattr(tty_fd, TCSANOW, &jobs[j].tmodes); // przywracamy termios joba
  }
  for (int i = 0; i < jobs[j].nproc; ++i)
    jobs[j].proc[i].state = RUNNING;
  jobs[j].state = RUNNING;
  kill(-jobs[j].pgid, SIGCONT);

  if (!bg) {
    monitorjob(mask);
  }

#endif /* !STUDENT */

  return true;
}

/* Kill the job by sending it a SIGTERM. */
bool killjob(int j) {
  if (j >= njobmax || jobs[j].state == FINISHED)
    return false;
  debug("[%d] killing '%s'\n", j, jobs[j].command);

  /* TODO: I love the smell of napalm in the morning. */
#ifdef STUDENT

  kill(-jobs[j].pgid, SIGTERM);
  kill(-jobs[j].pgid, SIGCONT);
  // jobs[j].state=FINISHED;
//  for (int i = 0; i < jobs[j].nproc; ++i) jobs[j].proc[i].state=FINISHED;
#endif /* !STUDENT */

  return true;
}

/* Report state of requested background jobs. Clean up finished jobs. */
void watchjobs(int which) {
  for (int j = BG; j < njobmax; j++) {
    if (jobs[j].pgid == 0)
      continue;

      /* TODO: Report job number, state, command and exit code or signal. */
#ifdef STUDENT
    int exitcode = -1;
    int status = jobstate(j, &exitcode);

    switch (status) {
      case RUNNING:
        if (which == RUNNING || which == ALL) {
          msg("[%d] running '%s'\n", j, jobs[j].command);
        }
        break;
      case STOPPED:
        if (which == STOPPED || which == ALL) {
          msg("[%d] suspended '%s'\n", j, jobs[j].command);
        }
        break;
      case FINISHED:
        if (which == FINISHED || which == ALL) {

          if (WIFEXITED(exitcode)) {
            msg("[%d] exited '%s', status=%d\n", j, jobs[j].command,
                WEXITSTATUS(exitcode));
          } else if (WIFSIGNALED(exitcode)) {
            msg("[%d] killed '%s' by signal %d\n", j, jobs[j].command,
                WTERMSIG(exitcode));
          } else
            fprintf(stderr, "%d - WTF?!\n",
                    exitcode); // do wywalenia, nie powinno sie nigdy zdarzyc
          deljob(&jobs[j]);
          break;
        }
    }
    (void)deljob;
#endif /* !STUDENT */
  }
}

/* Monitor job execution. If it gets stopped move it to background.
 * When a job has finished or has been stopped move shell to foreground. */
int monitorjob(sigset_t *mask) {
  int exitcode = 0, state;

  /* TODO: Following code requires use of Tcsetpgrp of tty_fd. */
#ifdef STUDENT
  // sigprocmask(SIG_SETMASK, mask, NULL);

  state = jobstate(0, &exitcode);
  // fprintf(stderr,"%d?!\n",state);
  while (state == RUNNING) {
    sigsuspend(mask);
    state = jobstate(0, &exitcode);
  }
  if (state == STOPPED)
    movejob(0, allocjob());

  setfgpgrp(getpgrp()); // przywracamy terminal i jego termios
  Tcsetattr(tty_fd, TCSANOW, &shell_tmodes);
  (void)jobstate;
  (void)exitcode;
  (void)state;
#endif /* !STUDENT */

  return exitcode;
}

/* Called just at the beginning of shell's life. */
void initjobs(void) {
  struct sigaction act = {
    .sa_flags = SA_RESTART,
    .sa_handler = sigchld_handler,
  };

  /* Block SIGINT for the duration of `sigchld_handler`
   * in case `sigint_handler` does something crazy like `longjmp`. */
  sigemptyset(&act.sa_mask);
  sigaddset(&act.sa_mask, SIGINT);
  Sigaction(SIGCHLD, &act, NULL);

  jobs = calloc(sizeof(job_t), 1);

  /* Assume we're running in interactive mode, so move us to foreground.
   * Duplicate terminal fd, but do not leak it to subprocesses that execve. */
  assert(isatty(STDIN_FILENO));
  tty_fd = Dup(STDIN_FILENO);
  fcntl(tty_fd, F_SETFD, FD_CLOEXEC);

  /* Take control of the terminal. */
  Tcsetpgrp(tty_fd, getpgrp());

  /* Save default terminal attributes for the shell. */
  Tcgetattr(tty_fd, &shell_tmodes);
}

/* Called just before the shell finishes. */
void shutdownjobs(void) {
  sigset_t mask;
  Sigprocmask(SIG_BLOCK, &sigchld_mask, &mask);

  /* TODO: Kill remaining jobs and wait for them to finish. */
#ifdef STUDENT
  for (int j = 0; j < njobmax; ++j) {
    killjob(j);
    jobs[j].state = FINISHED;
    for (int i = 0; i < jobs[j].nproc; ++i) {
      waitpid(jobs[j].proc[i].pid, &jobs[j].proc[i].exitcode, 0);
      jobs[j].proc[i].state = FINISHED;
    }
  }
#endif /* !STUDENT */

  watchjobs(FINISHED);

  Sigprocmask(SIG_SETMASK, &mask, NULL);

  Close(tty_fd);
}

/* Sets foreground process group to `pgid`. */
void setfgpgrp(pid_t pgid) {
  Tcsetpgrp(tty_fd, pgid);
}
