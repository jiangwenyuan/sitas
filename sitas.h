#ifndef SITAS_H
#define SITAS_H

#include <ncurses.h>
#include <termios.h>
#include <sys/ioctl.h>

typedef struct Session_t{
  char *host;
  pid_t pid;
  int pty;
  WINDOW *win;
  bool usable;
  int state;

  bool escape;
  int escape_len;
  char escape_buf[128];
  int sx,sy;

  bool bold;
  bool underscore;
  bool blink;
  bool reverse;
  bool concealed;//use bit
  int fc,bc;
} Session;

void parse_cmdline(int, char **);
void sitas_sigchild_handle(int);
void keyboard_init();
void color_init();
void sitas_init();
void session_init();
void sitas_connect();
void sitas_main_loop(); 
void sitas_user_event(); 
void sitas_child_event(); 
void sitas_splash();
void sitas_start();
void sitas_forward_key(int);
int sitas_sessions_num();
void sitas_list_session();
void sitas_add_session();
void sitas_prev_session();
void sitas_next_session();
void sitas_jump_session();
void sitas_visible_session ();
void sitas_switch_mode();
void sitas_help();
void sitas_quit();
void sitas_signal();
void sitas_child(int signo);
void session_new();
void session_free(Session *);
void sitas_update_sessions();
void sitas_update_info(char * ,...);
void sitas_banner();
void session_update(int, siginfo_t *, void *);
void session_print(Session *, char *, int);
void session_write2pty(int, int);
void Write(int, const char *, int);
void parse_escape(char *, int *);
void get_new_host (char *); 
void session_escape_attr(Session *, char);
char *trim_right(char *);
int setlock(int);
pid_t forkpty(int *, char *, struct termios *, struct winsize *);
void help();

#endif

