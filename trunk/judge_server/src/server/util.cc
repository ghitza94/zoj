#include <string>

#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/times.h>
#include <sys/wait.h>
#include <unistd.h>

#include "logging.h"
#include "util.h"
#include "kmmon-lib.h"

int setLimit(int resource, unsigned int limit) {
    struct rlimit t;
    t.rlim_max = limit + 1;
    t.rlim_cur = limit;
    return setrlimit(resource, &t);
}

double readTimeConsumption(int pid) {
    char buffer[64];
    sprintf(buffer, "/proc/%d/stat", pid);
    FILE* fp = fopen(buffer, "r");
    if (fp == NULL) {
        return -1;
    }
    int utime, stime;
    while (fgetc(fp) != ')');
    fgetc(fp);
    fscanf(fp,
           "%*c "
           "%*d %*d %*d %*d %*d "
           "%*u %*u %*u %*u %*u "
           "%d %d",
           &utime, &stime);
    fclose(fp);
    static int clktck = 0;
    if (clktck == 0) {
        clktck = sysconf(_SC_CLK_TCK);
    }
    return (utime + stime + 0.0) / clktck;
}

int readMemoryConsumption(int pid) {
    char buffer[64];
    sprintf(buffer, "/proc/%d/status", pid);
    FILE* fp = fopen(buffer, "r");
    if (fp == NULL) {
        return -1;
    }
    int vmPeak = 0, vmSize = 0, vmExe = 0, vmLib = 0;
    while (fgets(buffer, 32, fp)) {
        if (!strncmp(buffer, "VmPeak:", 7)) {
            sscanf(buffer + 7, "%d", &vmPeak);
        } else if (!strncmp(buffer, "VmSize:", 7)) {
            sscanf(buffer + 7, "%d", &vmSize);
        } else if (!strncmp(buffer, "VmExe:", 6)) {
            sscanf(buffer + 6, "%d", &vmExe);
        } else if (!strncmp(buffer, "VmLib:", 6)) {
            sscanf(buffer + 6, "%d", &vmLib);
        }
    }
    if (vmPeak) {
        vmSize = vmPeak;
    }
    return vmSize - vmExe - vmLib;
}

int createProcess(const char* commands[], const StartupInfo& processInfo) {
    const char* filename[] = {processInfo.stdinFilename,
                              processInfo.stdoutFilename,
                              processInfo.stderrFilename};
    int mode[] = {O_RDONLY, O_RDWR | O_CREAT | O_TRUNC, O_RDWR};
    int fd[] = {processInfo.fdStdin,
                processInfo.fdStdout,
                processInfo.fdStderr};
    for (int i = 0; i < 3; i++) {
        if (filename[i]) {
            fd[i] = open(filename[i], mode[i], 0777);
            if (fd[i] == -1) {
                LOG(SYSCALL_ERROR)<<"Fail to open "<<filename[i];
                for (int j = 0; j < i; j++) {
                    if (filename[j]) {
                        close(fd[j]);
                    }
                }
                return -1;
            }
        }
    }
    int pid = fork();
    if (pid < 0) {
        LOG(SYSCALL_ERROR);
        return -1;
    } if (pid > 0) {
        return pid;
    }
    for (int i = 0; i < 3; i++) {
        if (fd[i]) {
            if (dup2(fd[i], i) == -1) {
                LOG(SYSCALL_ERROR)<<"Fail to dup "<<fd[i]<<" to "<<i;
                raise(SIGKILL);
            }
            close(fd[i]);
        }
    }
    for (int i = 3; i < 100; i++) {
        close(i);
    }
    if (processInfo.timeLimit) {
        if (setLimit(RLIMIT_CPU, processInfo.timeLimit) == -1) {
            LOG(SYSCALL_ERROR)<<"Fail to set cpu limit to "
                              <<processInfo.timeLimit<<'s';
            raise(SIGKILL);
        }
    }
    if (processInfo.memoryLimit) {
        if (setLimit(RLIMIT_DATA, processInfo.memoryLimit * 1024) == -1) {
            LOG(SYSCALL_ERROR)<<"Fail to set memory limit to "
                              <<processInfo.memoryLimit<<'k';
            raise(SIGKILL);
        }
    }
    if (processInfo.outputLimit) {
        if (setLimit(RLIMIT_FSIZE, processInfo.outputLimit * 1024) == -1) {
            LOG(SYSCALL_ERROR)<<"Fail to set output limit to "
                              <<processInfo.outputLimit<<'k';
            raise(SIGKILL);
        }
    }
    if (processInfo.fileLimit) {
        if (setLimit(RLIMIT_NOFILE, processInfo.fileLimit) == -1) {
            LOG(SYSCALL_ERROR)<<"Fail to set file limit to "
                              <<processInfo.fileLimit;
            raise(SIGKILL);
        }
    }
    if (processInfo.workingDirectory) {
        if (chdir(processInfo.workingDirectory) == -1) {
            LOG(SYSCALL_ERROR)<<"Fail to change working directory to "
                              <<processInfo.workingDirectory;
            raise(SIGKILL);
        }
    }
    if (processInfo.gid) {
        if (setgid(processInfo.gid) == -1) {
            LOG(SYSCALL_ERROR)<<"Fail to set gid to "<<processInfo.gid;
            raise(SIGKILL);
        }
    }
    if (processInfo.uid) {
        if (setuid(processInfo.uid) == -1) {
            LOG(SYSCALL_ERROR)<<"Fail to set uid to "<<processInfo.uid;
            raise(SIGKILL);
        }
    }
    if (processInfo.procLimit) {
        if (setLimit(RLIMIT_NPROC, processInfo.procLimit) == -1) {
            LOG(SYSCALL_ERROR)<<"Fail to set process limit to "
                              <<processInfo.procLimit;
            raise(SIGKILL);
        }
    }
    if (processInfo.trace) {
        if (kmmon_traceme() == -1) {
            LOG(SYSCALL_ERROR)<<"Fail to trace";
            raise(SIGKILL);
        }
    }
    if (execv(commands[0], (char**)(commands + 1)) == -1) {
        LOG(SYSCALL_ERROR)<<"Fail to execute command '"<<commands[0]<<"'";
        raise(SIGKILL);
    }
    return -1;
}

int createShellProcess(const char* command, const StartupInfo& processInfo) {
    const char* commands[] = {"/bin/sh", "sh", "-c", command, NULL};
    return createProcess(commands, processInfo);
}

ssize_t readn(int fd, void* buffer, size_t count) {
	char* p = (char*)buffer;
	while (count > 0) {
		ssize_t num = read(fd, p, count);
		if (num == -1) {
			if (errno == EINTR) {
				// interrupted by a signals, read again
				continue;
			}
			LOG(SYSCALL_ERROR)<<"Fail to read from file";
			return -1;
		}
		if (num == 0) {
			// EOF
			break;
		}
		p += num;
		count -= num;
	}
	return p - (char*)buffer;
}

int writen(int fd, const void* buffer, size_t count) {
	const char*p = (const char*)buffer;
	while (count > 0) {
		int num = write(fd, p, count);
		if (num == -1) {
			if (errno == EINTR) {
				// interrupted by a signals, write again
				continue;
			}
			LOG(SYSCALL_ERROR)<<"Fail to write to file";
			return -1;
		}
		p += num;
		count -= num;
	}
	return 0;
}

int copyFile(int fdSource, int fdDestination) {
    char buffer[1024];
    for (;;) {
        int count = readn(fdSource, buffer, sizeof(buffer));
        if (count == -1) {
            return -1;
        }
        if (writen(fdDestination, buffer, count) == -1) {
            return -1;
        }
        if (count < (int) sizeof(buffer)) {
            return 0;
        }
    }
}

int saveFile(int fdSource, const std::string& outputFilename) {
    int fdDestination = open(outputFilename.c_str(),
                             O_RDWR | O_CREAT | O_TRUNC);
    if (fdDestination == -1) {
        LOG(SYSCALL_ERROR)<<"Fail to open file "<<outputFilename;
        return -1;
    }
    int ret = copyFile(fdSource, fdDestination);
    close(fdDestination);
    return ret;
}

int sendReply(int fdSocket, int reply) {
	char buffer[16];
	sprintf(buffer, "%d\n", reply);
	return writen(fdSocket, buffer, strlen(buffer));
}

sighandler_t installSignalHandler(int signal, sighandler_t handler) {
    return installSignalHandler(signal, handler, 0);
}

sighandler_t installSignalHandler(int signal, sighandler_t handler, int flags) {
    sigset_t mask;
    sigemptyset(&mask);
    return installSignalHandler(signal, handler, flags, mask);
}

sighandler_t installSignalHandler(
        int signal, sighandler_t handler, int flags, sigset_t mask) {
    struct sigaction act, oact;
    act.sa_handler = handler;
    act.sa_mask = mask;
    act.sa_flags = flags;
    if (signal == SIGALRM) {
#ifdef SA_INTERRUPT
        act.sa_flags |= SA_INTERRUPT;
#endif
    } else {
#ifdef SA_RESTART
        act.sa_flags |= SA_RESTART;
#endif
    }
    if (sigaction(signal, &act, &oact) < 0) {
        return SIG_ERR;
    }
    return oact.sa_handler;
}

void daemonize() {
    umask(0);
    int pid = fork();
    if (pid < 0) {
        LOG(SYSCALL_ERROR);
        exit(1);
    } else if (pid > 0) {
        exit(0);
    }

    // start a new session
    setsid();

    // ignore SIGHUP
    if (installSignalHandler(SIGHUP, SIG_IGN) == SIG_ERR) {
        LOG(SYSCALL_ERROR)<<"Fail to ignore SIGHUP";
        exit(1);
    }
    
    // attach file descriptor 0, 1, 2 to /dev/null
    int fd = open("/dev/null", O_RDWR);
    dup2(fd, 0);
    dup2(fd, 1);
    dup2(fd, 2);

    // close all other file descriptors
    for (int i = 3; i < 100; i++) {
        close(i);
    }
}

int createServerSocket(int port) {
    int sockServer = socket(PF_INET, SOCK_STREAM, 6);
    if (sockServer == -1) {
        LOG(SYSCALL_ERROR)<<"Fail to create socket";
        return -1;
    }
    int optionValue = 1;
    if (setsockopt(sockServer, 
                   SOL_SOCKET,
                   SO_REUSEADDR,
                   &optionValue,
                   sizeof(optionValue)) == -1) {
        LOG(SYSCALL_ERROR)<<"Fail to set socket option";
        return -1;
    }
    sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY); 
    address.sin_port = htons(port);
    if (bind(sockServer, (struct sockaddr*)&address, sizeof(address)) == -1) {
        LOG(SYSCALL_ERROR)<<"Fail to bind";
        return -1;
    }
    if (listen(sockServer, 32) == -1) {
        LOG(SYSCALL_ERROR)<<"Fail to listen";
        return -1;
    }
    return sockServer;
}

int lockFile(int fd, int cmd) {
    struct flock lock;
    lock.l_type = F_WRLCK;
    lock.l_start = 0;
    lock.l_whence = SEEK_SET;
    lock.l_len = 0;
    return fcntl(fd, cmd, &lock);
}

