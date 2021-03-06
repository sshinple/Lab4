/*
 * tsh - A tiny shell program with job control
 *
 * Seung Heon Shin Brian (shs522) & Navya Suri (ns3774)
 * shs522+ns3774
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>

/* Misc manifest constants */
#define MAXLINE 1024   /* max line size */
#define MAXARGS 128    /* max args on a command line */
#define MAXJOBS 16     /* max jobs at any point in time */
#define MAXJID 1 << 16 /* max job ID */

/* Job states */
#define UNDEF 0 /* undefined */
#define FG 1    /* running in foreground */
#define BG 2    /* running in background */
#define ST 3    /* stopped */

/*
 * Jobs states: FG (foreground), BG (background), ST (stopped)
 * Job state transitions and enabling actions:
 *     FG -> ST  : ctrl-z
 *     ST -> FG  : fg command
 *     ST -> BG  : bg command
 *     BG -> FG  : fg command
 * At most 1 job can be in the FG state.
 */

/* Global variables */
extern char **environ;   /* defined in libc */
char prompt[] = "tsh> "; /* command line prompt (DO NOT CHANGE) */
int verbose = 0;         /* if true, print additional output */
int nextjid = 1;         /* next job ID to allocate */
char sbuf[MAXLINE];      /* for composing sprintf messages */

struct job_t
{                          /* The job struct */
    pid_t pid;             /* job PID */
    int jid;               /* job ID [1, 2, ...] */
    int state;             /* UNDEF, BG, FG, or ST */
    char cmdline[MAXLINE]; /* command line */
};
struct job_t jobs[MAXJOBS]; /* The job list */
/* End global variables */

/* Function prototypes */

/* Here are the functions that you will implement */
void eval(char *cmdline);     // Main routine that parses and interprets the command line
int builtin_cmd(char **argv); //Recognizes and interprets the built-in commands: quit, fg, bg, and jobs
void do_bgfg(char **argv);    // Implements the bg and fg built-in commands
void waitfg(pid_t pid);       // Waits for a foreground job to complete

void sigchld_handler(int sig); // Catches SIGCHLD signals
void sigtstp_handler(int sig); // Catches SIGINT (cntrl-c) signals
void sigint_handler(int sig);  // Catches SIGTSTP (cntrl-z) signals

/* Here are helper routines that we've provided for you */
int parseline(const char *cmdline, char **argv);
void sigquit_handler(int sig);

void clearjob(struct job_t *job);
void initjobs(struct job_t *jobs);
int maxjid(struct job_t *jobs);
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline);
int deletejob(struct job_t *jobs, pid_t pid);
pid_t fgpid(struct job_t *jobs);
struct job_t *getjobpid(struct job_t *jobs, pid_t pid);
struct job_t *getjobjid(struct job_t *jobs, int jid);
int pid2jid(pid_t pid);
void listjobs(struct job_t *jobs);

void usage(void);
void unix_error(char *msg);
void app_error(char *msg);
typedef void handler_t(int);
handler_t *Signal(int signum, handler_t *handler);

// Additionally Implemented Functions
void _setpgid(pid_t pid, pid_t pgid);
void _sigemptyset(sigset_t *set);
void _sigaddset(sigset_t *set, int sig);
void _sigprocmask(int sig, sigset_t *curSet, sigset_t *prevSet);
pid_t _fork(void);

/*
 * main - The shell's main routine
 */
int main(int argc, char **argv)
{
    char c;
    char cmdline[MAXLINE];
    int emit_prompt = 1; /* emit prompt (default) */

    /* Redirect stderr to stdout (so that driver will get all output
     * on the pipe connected to stdout) */
    dup2(1, 2);

    /* Parse the command line */
    while ((c = getopt(argc, argv, "hvp")) != EOF)
    {
        switch (c)
        {
        case 'h': /* print help message */
            usage();
            break;
        case 'v': /* emit additional diagnostic info */
            verbose = 1;
            break;
        case 'p':            /* don't print a prompt */
            emit_prompt = 0; /* handy for automatic testing */
            break;
        default:
            usage();
        }
    }

    /* Install the signal handlers */

    /* These are the ones you will need to implement */
    Signal(SIGINT, sigint_handler);   /* ctrl-c */
    Signal(SIGTSTP, sigtstp_handler); /* ctrl-z */
    Signal(SIGCHLD, sigchld_handler); /* Terminated or stopped child */

    /* This one provides a clean way to kill the shell */
    Signal(SIGQUIT, sigquit_handler);

    /* Initialize the job list */
    initjobs(jobs);

    /* Execute the shell's read/eval loop */
    while (1)
    {

        /* Read command line */
        if (emit_prompt)
        {
            printf("%s", prompt);
            fflush(stdout);
        }
        if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin))
            app_error("fgets error");
        if (feof(stdin))
        { /* End of file (ctrl-d) */
            fflush(stdout);
            exit(0);
        }

        /* Evaluate the command line */
        eval(cmdline);
        fflush(stdout);
        fflush(stdout);
    }

    exit(0); /* control never reaches here */
}

/*
 * eval - Evaluate the command line that the user has just typed in
 *
 * If the user has requested a built-in command (quit, jobs, bg or fg)
 * then execute it immediately. Otherwise, fork a child process and
 * run the job in the context of the child. If the job is running in
 * the foreground, wait for it to terminate and then return.  Note:
 * each child process must have a unique process group ID so that our
 * background children don't receive SIGINT (SIGTSTP) from the kernel
 * when we type ctrl-c (ctrl-z) at the keyboard.
*/
void eval(char *cmdline)
{
    // The parent must use sigprocmask to block SIGCHLD signals before it forks the child
    // Afterwards we unblock the signals by using sigprocmask after it adds the child to the job list by calling addjob
    // Since children inherit the blocked vectors of their parents, the child must be sure to then unblock SIGCHLD signals before it execs the new program

    // After the fork, but before the execve, the child process should call setpgid(0,0), which puts the child in a new process group whose group ID is identical to the child's PID
    // ** This ensures that there will be only one process, your shell, in the foreground process group

    char *argv[MAXARGS]; // List of Arguments
    pid_t pid;           // Process ID
    sigset_t mask;       // Blocking Signals
    int bg;              // Foreground (fg) / Background (bg) - parseline returns 1 for bg

    // Parse the command line and build the argv array.
    bg = parseline(cmdline, argv);

    // If there is no first argument (meaning that the user has just pressed ENTER), don't do anything - display new prompt
    if (argv[0] == NULL)
    {
        return;
    }

    // Evaluating whether argument is valid builtin_cmd
    if (!builtin_cmd(argv))
    {
        // Initially blocking SIGCHLD
        _sigemptyset(&mask);                  // Initializing signal set
        _sigaddset(&mask, SIGCHLD);           // Adding SIGCHLD to signal set
        _sigprocmask(SIG_BLOCK, &mask, NULL); // Adding singals to SIG_BLOCK

        // Forking Child Process
        if ((pid = _fork()) == 0)
        {
            _setpgid(0, 0);                         // Setting child's group
            _sigprocmask(SIG_UNBLOCK, &mask, NULL); // Unblocking SIGCHLD

            // Checking command
            if (execve(argv[0], argv, environ) < 0)
            {
                printf("Command not found: %s\n", argv[0]);
                exit(1);
            }
            return;
        }

        // Parent
        else
        {
            addjob(jobs, pid, bg ? BG : FG, cmdline); // Adding process to job list, depending on BG/FG
            _sigprocmask(SIG_UNBLOCK, &mask, NULL);   // Retrieving SIGCHLD signal by unblocking

            if (!bg)
            {
                waitfg(pid); // Reaping when job is Terminated
            }
            else
            {
                printf("[%d] (%d) %s", pid2jid(pid), pid, cmdline); // Printing bg process info
            }
            return;
        }
    }
    return;
}

/*
 * parseline - Parse the command line and build the argv array.
 *
 * Characters enclosed in single quotes are treated as a single
 * argument.  Return true if the user has requested a BG job, false if
 * the user has requested a FG job.
 */
int parseline(const char *cmdline, char **argv)
{
    static char array[MAXLINE]; /* holds local copy of command line */
    char *buf = array;          /* ptr that traverses command line */
    char *delim;                /* points to first space delimiter */
    int argc;                   /* number of args */
    int bg;                     /* background job? */

    strcpy(buf, cmdline);
    buf[strlen(buf) - 1] = ' ';   /* replace trailing '\n' with space */
    while (*buf && (*buf == ' ')) /* ignore leading spaces */
        buf++;

    /* Build the argv list */
    argc = 0;
    if (*buf == '\'')
    {
        buf++;
        delim = strchr(buf, '\'');
    }
    else
    {
        delim = strchr(buf, ' ');
    }

    while (delim)
    {
        argv[argc++] = buf;
        *delim = '\0';
        buf = delim + 1;
        while (*buf && (*buf == ' ')) /* ignore spaces */
            buf++;

        if (*buf == '\'')
        {
            buf++;
            delim = strchr(buf, '\'');
        }
        else
        {
            delim = strchr(buf, ' ');
        }
    }
    argv[argc] = NULL;

    if (argc == 0) /* ignore blank line */
        return 1;

    /* should the job run in the background? */
    if ((bg = (*argv[argc - 1] == '&')) != 0)
    {
        argv[--argc] = NULL;
    }
    return bg;
}

/*
 * builtin_cmd - If the user has typed a built-in command then execute
 *    it immediately.
 */
int builtin_cmd(char **argv)
{
    // Comapre input to "quit"
    if (!strcmp(argv[0], "quit"))
    {
        if (verbose)
        {
            printf("Quit Command Detected\n");
        }
        exit(0);
    }

    // Comapre input to "jobs"
    else if (!strcmp(argv[0], "jobs"))
    {
        if (verbose)
        {
            printf("Jobs Command Detected\n");
        }
        listjobs(jobs);
        return 1;
    }

    // Comapre input to "bg" or "fg"
    else if (!strcmp(argv[0], "bg") || !strcmp(argv[0], "fg"))
    {
        if (verbose)
        {
            printf("Running FG/BG Job\n");
        }
        do_bgfg(argv);
        return 1;
    }

    // Not a built in command
    return 0;
}

/*
 * do_bgfg - Execute the builtin bg and fg commands
 */
void do_bgfg(char **argv)
{
    struct job_t *job;
    char *id;
    int jid = 0;
    pid_t pid;

    id = argv[1];

    // Invalid ID
    if (id == NULL)
    {
        printf("%s command requires PID or %%jobid argument\n", argv[0]);
        return;
    }

    // We will first get the job from the supplied jid or pid and then change the state later. 
    
    if (id[0] == '%')
    {
        // Here, user has supplied JID. We get the corresponding job. 
        jid = atoi(&id[1]);
        job = getjobjid(jobs, jid); // Getting Job

        // If the job returned is NULL or the jid is 0 (no jid should be zero) - we return with error msg
        if (job==NULL || jid==0)
        {
            //%2: No such job
            printf("%s: No such job\n", id);
            return;
        }
    }

    // The other option is that user has supplied PID. We get the corresponding job. 
    else if (isdigit(id[0]))
    {
        pid = atoi(id);
        job = getjobpid(jobs, pid);

        // Again, if job returned is NULL or pid is 0, we return with error msg. 
        if (job==NULL || pid==0)
        {
            //(2): No such process
            printf("(%d): No such process\n", pid);
            return;
        }
    }

    // The default case is that user has supplied some garbage value for the jid/pid starting without digit or %. 
    else
    {
        printf("%s: argument must be PID of %%jobid\n", argv[0]);
        return;
    }

    pid = job->pid;

    // Resuming program when SIGCONT received
    kill(-pid, SIGCONT);

    // Now that we have the job, we can "do" fg/bg accordingly
    if (!strcmp("fg", argv[0]))
    {
        // This command moves BG to FG. Change state and call waitfg. 
        job->state = FG;
        waitfg(job->pid);
    }
    else
    {
        // This command moves FG to BG. Change state and print that jobs is now in BG. 
        job->state = BG;
        printf("[%d] (%d) %s\n", job->jid, job->pid, job->cmdline);
    }

    return;
}

/*
 * waitfg - Block until process pid is no longer the foreground process
 */
void waitfg(pid_t pid)
{
    // First, we get the FG job
    struct job_t *fg_job = getjobpid(jobs, pid);

    if (verbose)
        printf("Waiting FG\n");

    if (pid == 0)
    {
        return;
    }

    // Wait until FG job is finished using sleep()
    if (fg_job != NULL)
    {
        while (pid == fgpid(jobs))
        {
            sleep(1);
        }
    }
    return;
}

/*****************
 * Signal handlers
 *****************/

/*
 * sigchld_handler - The kernel sends a SIGCHLD to the shell whenever
 *     a child job terminates (becomes a zombie), or stops because it
 *     received a SIGSTOP or SIGTSTP signal. The handler reaps all
 *     available zombie children, but doesn't wait for any other
 *     currently running children to terminate.
 */
void sigchld_handler(int sig)
{
    pid_t pid;
    struct job_t *job;
    int status; // this status is set by the waitpid function

    if (verbose)
    {
        printf("SIGCHLD signal recieved\n");
    }

    // Here we modify default behviour of the waitpid function.
    // We pass an integer which gets the status of the child.
    // The mode is set to WNOHANG|WUNTRACED - returns the pid of one of the
    //  stopped or terminated children, 0 if none. (from textbook)
    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0)
    {
        // Now that we have status of the child, we can either delete, or change state.
        job = getjobpid(jobs, pid);
        if (WIFEXITED(status))
        {
            // Child terminated normally. So, delete the job from the list.
            if (verbose)
                printf("   Child exited normally\n");
            deletejob(jobs, pid);
        }
        else if (WIFSIGNALED(status))
        {
            // Child terminated because of an uncaught signal. So, delete the job from the list.
            // Also, according to reference solution, we must print the signal which caused the termination. (use WTERMSIG)
            int terminator = WTERMSIG(status);
            printf("JOB [%d] (%d) terminated by SIGNAL %d\n", pid2jid(pid), pid, terminator);
            deletejob(jobs, pid);
        }

        else if (WIFSTOPPED(status))
        {
            // Child is currently stopped. No need to delete.
            job->state = ST; // Set the state to ST (stopped)
            // According to reference solution, we should print the Signal that caused the stop. (use WSTOPSIG)
            int stopper = WSTOPSIG(status);
            printf("JOB [%d] (%d) stopped by SIGNAL %d\n", pid2jid(pid), pid, stopper);
        }
    }

    // Detect Error and print accordingly (got this part from the textbook)
    // if (errno == ECHILD)
    // {
    //     unix_error("Error in WaitPID\n");
    // }

    return;
}

/*
 * sigint_handler - The kernel sends a SIGINT to the shell whenver the
 *    user types ctrl-c at the keyboard.  Catch it and send it along
 *    to the foreground job.
 */
void sigint_handler(int sig)
{
    // printf("Interrupt Signal\n");

    // Get the process ID and send the SIGINT signal using kill function
    pid_t pid = fgpid(jobs);
    if (pid != 0)
    {
        if (verbose)
        {
            printf("Terminating the foreground job\n");
        }

        kill(-pid, SIGINT);
    }

    return;
}

/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 *     the user types ctrl-z at the keyboard. Catch it and suspend the
 *     foreground job by sending it a SIGTSTP.
 */
void sigtstp_handler(int sig)
{
    // printf(" Suspend signal\n");

    // Get the process ID and send the signal using kill function
    pid_t pid = fgpid(jobs);
    if (pid != 0)
    {
        if (verbose)
        {
            printf("Suspending the forground job\n");
        }

        kill(-pid, SIGTSTP);
    }

    return;
}

/*********************
 * End signal handlers
 *********************/

/***********************************************
 * Helper routines that manipulate the job list
 **********************************************/

/* clearjob - Clear the entries in a job struct */
void clearjob(struct job_t *job)
{
    job->pid = 0;
    job->jid = 0;
    job->state = UNDEF;
    job->cmdline[0] = '\0';
}

/* initjobs - Initialize the job list */
void initjobs(struct job_t *jobs)
{
    int i;

    for (i = 0; i < MAXJOBS; i++)
        clearjob(&jobs[i]);
}

/* maxjid - Returns largest allocated job ID */
int maxjid(struct job_t *jobs)
{
    int i, max = 0;

    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i].jid > max)
            max = jobs[i].jid;
    return max;
}

/* addjob - Add a job to the job list */
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline)
{
    int i;

    if (pid < 1)
        return 0;

    for (i = 0; i < MAXJOBS; i++)
    {
        if (jobs[i].pid == 0)
        {
            jobs[i].pid = pid;
            jobs[i].state = state;
            jobs[i].jid = nextjid++;
            if (nextjid > MAXJOBS)
                nextjid = 1;
            strcpy(jobs[i].cmdline, cmdline);
            if (verbose)
            {
                printf("Added job [%d] %d %s\n", jobs[i].jid, jobs[i].pid, jobs[i].cmdline);
            }
            return 1;
        }
    }
    printf("Tried to create too many jobs\n");
    return 0;
}

/* deletejob - Delete a job whose PID=pid from the job list */
int deletejob(struct job_t *jobs, pid_t pid)
{
    int i;

    if (pid < 1)
        return 0;

    for (i = 0; i < MAXJOBS; i++)
    {
        if (jobs[i].pid == pid)
        {
            clearjob(&jobs[i]);
            nextjid = maxjid(jobs) + 1;
            return 1;
        }
    }
    return 0;
}

/* fgpid - Return PID of current foreground job, 0 if no such job */
pid_t fgpid(struct job_t *jobs)
{
    int i;

    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i].state == FG)
            return jobs[i].pid;
    return 0;
}

/* getjobpid  - Find a job (by PID) on the job list */
struct job_t *getjobpid(struct job_t *jobs, pid_t pid)
{
    int i;

    if (pid < 1)
        return NULL;
    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i].pid == pid)
            return &jobs[i];
    return NULL;
}

/* getjobjid  - Find a job (by JID) on the job list */
struct job_t *getjobjid(struct job_t *jobs, int jid)
{
    int i;

    if (jid < 1)
        return NULL;
    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i].jid == jid)
            return &jobs[i];
    return NULL;
}

/* pid2jid - Map process ID to job ID */
int pid2jid(pid_t pid)
{
    int i;

    if (pid < 1)
        return 0;
    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i].pid == pid)
        {
            return jobs[i].jid;
        }
    return 0;
}

/* listjobs - Print the job list */
void listjobs(struct job_t *jobs)
{
    int i;

    for (i = 0; i < MAXJOBS; i++)
    {
        if (jobs[i].pid != 0)
        {
            printf("[%d] (%d) ", jobs[i].jid, jobs[i].pid);
            switch (jobs[i].state)
            {
            case BG:
                printf("Running ");
                break;
            case FG:
                printf("Foreground ");
                break;
            case ST:
                printf("Stopped ");
                break;
            default:
                printf("listjobs: Internal error: job[%d].state=%d ",
                       i, jobs[i].state);
            }
            printf("%s", jobs[i].cmdline);
        }
    }
}
/******************************
 * end job list helper routines
 ******************************/

/***********************
 * Other helper routines
 ***********************/

/*
 * usage - print a help message
 */
void usage(void)
{
    printf("Usage: shell [-hvp]\n");
    printf("   -h   print this message\n");
    printf("   -v   print additional diagnostic information\n");
    printf("   -p   do not emit a command prompt\n");
    exit(1);
}

/*
 * unix_error - unix-style error routine
 */
void unix_error(char *msg)
{
    fprintf(stdout, "%s: %s\n", msg, strerror(errno));
    exit(1);
}

/*
 * app_error - application-style error routine
 */
void app_error(char *msg)
{
    fprintf(stdout, "%s\n", msg);
    exit(1);
}

/*
 * Signal - wrapper for the sigaction function
 */
handler_t *Signal(int signum, handler_t *handler)
{
    struct sigaction action, old_action;

    action.sa_handler = handler;
    sigemptyset(&action.sa_mask); /* block sigs of type being handled */
    action.sa_flags = SA_RESTART; /* restart syscalls if possible */

    if (sigaction(signum, &action, &old_action) < 0)
        unix_error("Signal error");
    return (old_action.sa_handler);
}

/*
 * sigquit_handler - The driver program can gracefully terminate the
 *    child shell by sending it a SIGQUIT signal.
 */
void sigquit_handler(int sig)
{
    printf("Terminating after receipt of SIGQUIT signal\n");
    exit(1);
}

void _setpgid(pid_t pid, pid_t pgid)
{
    if (setpgid(pid, pgid) < 0)
    {
        unix_error("setpgid error");
    }
}

void _sigemptyset(sigset_t *set)
{
    if (sigemptyset(set) < 0)
    {
        app_error("sigemptyset error\n");
    }
}
void _sigaddset(sigset_t *set, int sig)
{
    if (sigaddset(set, sig) < 0)
    {
        app_error("sigaddset error\n");
    }
}
void _sigprocmask(int sig, sigset_t *curSet, sigset_t *prevSet)
{
    if (sigprocmask(sig, curSet, prevSet) < 0)
    {
        app_error("sigprocmask error\n");
    }
}

pid_t _fork(void)
{
    pid_t pid;

    if ((pid = fork()) < 0)
    {
        unix_error("Fork error");
    }

    return pid;
}
