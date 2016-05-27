#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/mman.h>
#define __USE_XOPEN
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <stdarg.h>
#include <locale.h>

#include "sitas.h"

#define STDIN   STDIN_FILENO
#define STDOUT  STDOUT_FILENO
#define STDERR  STDERR_FILENO

#define PROGRAM "sitas"
#define VERSION "0.01"
#define COPYRIGHT "Jiang WenYuan"

#define SIG_UPDATE      SIGRTMAX
#define SIG_CON         SIGRTMAX-1
#define MAX_BUFFER      1024
#define LOCK            1
#define UNLOCK          0
#define MAX_ESCAPE_ARGC 8

#define NOTCONNECTED    0
#define CONNECTING      1
#define CONNECTED       2
#define DISCONNECTED    3
#define CONNECTFAILED   4 

int MAX_SESSIONS = 128;

FILE *wfile = NULL;

int SCRROWS, SCRCOLS;
int TERMROWS, TERMCOLS;
int PTYROWS, PTYCOLS;
WINDOW *infowin = NULL;
WINDOW *termwin = NULL;
WINDOW *helpwin = NULL;
WINDOW *listwin = NULL;
char *skey[KEY_MAX + 1];
int color_map[8][8];
Session (*session)[1];
char **hosts = NULL;
int hosts_num = 0;
bool multi = true;
int current = 0;
struct flock *lock;
int lockfd;
static volatile sig_atomic_t sigflag;
bool insmod = false;
bool insnum = 0;
int prev_key=-1;
bool control_mode = false;

void parse_cmdline (int argc, char **argv) {
  int optch;
  char *hosts_file = NULL;
  while ((optch = getopt (argc, argv, "f:hm:v")) != -1) {
    switch (optch) {
      case 'f':
        hosts_file = optarg;
        break;
      case 'm':
        MAX_SESSIONS = atoi (optarg);
        if (MAX_SESSIONS <= 0) {
          MAX_SESSIONS = 128;
        }
        break;
      case 'v':
        printf ("%s %s\t%s.\n", PROGRAM, VERSION, COPYRIGHT);
        exit (1);
      case 'h':
        help ();
        exit (1);
      default:
        help ();
        exit (1);
    }
  }

  if (hosts_file == NULL) {
    help ();
    exit(1);
  }

  FILE *pfile = fopen (hosts_file, "r");
  if (pfile == NULL) {
    printf ("Error: cannot open file `%s`\n", hosts_file);
    exit (1);
  }

  int len = 0;
  char buf[4096];
  memset (buf, 0, 4096);
  int num = 0;
  while (!feof (pfile)) {
    if (fgets (buf, 4096, pfile) != NULL) {
      len = strlen (trim_right (buf));
      if (len == 0) {
        continue;
      }
      char *tmp = malloc (len + 1);
      memset (tmp, 0, len + 1);
      memcpy (tmp, buf, len);
      num = hosts_num;
      hosts_num++;
      hosts = (char **) realloc (hosts, sizeof (char *) * hosts_num);
      if (hosts == NULL) {
        break;
      } else {
        *(hosts + num) = malloc (sizeof (char));
        memset (*(hosts + num), 0, 4);
        *(hosts + num) = tmp;
      }
    }
  }
  if (hosts == NULL || hosts_num == 0) {
    printf ("Error: cannot parse file: `%s`\n", hosts_file);
    exit (1);
  }
  if (hosts_num > MAX_SESSIONS) {
    printf ("Error: too many hosts.\n");
    exit (1);
  }
}

void sitas_init () {
  setlocale(LC_ALL, "");
  initscr ();
  keyboard_init ();
  color_init ();
  noecho ();
  cbreak ();
  clear ();

  getmaxyx (stdscr, SCRROWS, SCRCOLS);
  TERMROWS = SCRROWS - 2;
  TERMCOLS = SCRCOLS;
  PTYROWS = TERMROWS;
  PTYCOLS = TERMCOLS - 1;
  if (SCRROWS < 25 || SCRCOLS < 80) {
    endwin ();
    printf ("ERROR: need at least 80x25 terminal.\n");
    exit (1);
  }

  refresh ();
  infowin = newwin (SCRROWS - TERMROWS, SCRCOLS, 0, 0);
  termwin = newwin (TERMROWS, TERMCOLS, SCRROWS - TERMROWS, 0);
  scrollok (termwin, true);
  sitas_signal ();


  /*init lock */
  /*
     char tmpfile[] = "tmp_XXXXXX";
     do{
     lockfd = mkstemp(tmpfile);
     }while(lockfd  ==  -1);
     unlink(tmpfile);
     lock = (struct flock *)mmap(NULL, sizeof(struct flock), PROT_READ|PROT_WRITE, MAP_SHARED|MAP_ANONYMOUS, -1, 0);

     wattrset(stdscr, COLOR_PAIR(3)|A_BOLD);
     wmove(stdscr, 0, 0);
     whline(stdscr, ACS_HLINE|A_NORMAL, cols);
     leaveok(termwin, TRUE);
     */
}

void keyboard_init () {
  keypad (stdscr, true);
  memset (skey, 0, KEY_MAX + 1 * sizeof (char *));

  skey[KEY_UP]          = "\033[A";
  skey[KEY_DOWN]        = "\033[B";
  skey[KEY_RIGHT]       = "\033[C";
  skey[KEY_LEFT]        = "\033[D";
  skey[KEY_BACKSPACE]   = "\b";
  /*skey[KEY_HOME]       =  "\033[H"; */
  skey[KEY_HOME]        = "\033[1~";
  skey[KEY_IC]          = "\033[2~";
  skey[KEY_DC]          = "\033[3~";
  /*skey[KEY_END]        =  "\033[G"; */
  skey[KEY_END]         = "\033[4~";
  skey[KEY_PPAGE]       = "\033[5~";
  skey[KEY_NPAGE]       = "\033[6~";
}

void color_init () {
  start_color ();
  int fc, bc, pair = 0;
  for (fc = 0; fc < 8; fc++) {
    for (bc = 0; bc < 8; bc++) {
      pair++;
      color_map[fc][bc] = pair;
      init_pair (pair, fc, bc);
    }
  }
}

//for Mac OS X
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
void session_init () {
  Session *s;
  s = (Session *) mmap (NULL, sizeof (Session) * MAX_SESSIONS, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);

  session = (Session (*)[1]) s;
  int i = 0;
  for (i = 0; i < MAX_SESSIONS; i++) {
    if (i < hosts_num) {
      int len = strlen (*(hosts + i));
      session[i]->host = malloc (len + 1);
      memset (session[i]->host, 0, len + 1);
      memcpy (session[i]->host, *(hosts + i), len);
    } else {
      session[i]->host = NULL;
    }
    session[i]->pid = -1;
    session[i]->pty = -1;
    session[i]->win = NULL;
    session[i]->usable = true;
    session[i]->state = NOTCONNECTED;
    session[i]->bold = false;
    session[i]->underscore = false;
    session[i]->blink = false;
    session[i]->reverse = false;
    session[i]->concealed = false;	/* may use bit */
    session[i]->fc = COLOR_WHITE;
    session[i]->bc = COLOR_BLACK;
  }
  
}

void sitas_splash () {
  wprintw (infowin, "%s %s\t%s.", PROGRAM, VERSION, COPYRIGHT);
  mvwchgat (infowin, 0, 0, 10, 0, color_map[COLOR_GREEN][COLOR_BLACK], NULL);
//  mvwchgat (infowin, 1, 0, -1, 0, color_map[COLOR_WHITE][COLOR_BLUE], NULL);
  mvwprintw (termwin, 0, 0, "Connecting...Please wait..(ctrl-c to abort)\n");
  mvwprintw (termwin, 1, 0, "Press L to see connceting status\n");
  wrefresh (infowin);
  wrefresh (termwin);

  while(1) {
    // check if still connecting
    bool connecting = false;
    int i=0;
    for (i = 0; i < MAX_SESSIONS; i++) {
      if (session[i] != NULL && session[i]->usable != true) {
        if (session[i]->state == CONNECTING){
          connecting = true;
          break;
        }
      }
    }
    // no.
    if (connecting == false){
      break;
    }

    fd_set rfs;
    FD_ZERO (&rfs);
    FD_SET (STDIN, &rfs);
    int max = STDIN;
    for (i = 0; i < MAX_SESSIONS; i++) {
      if (session[i] != NULL && session[i]->usable != true && session[i]->state == CONNECTING) {
        FD_SET (session[i]->pty, &rfs);
        if (max < session[i]->pty) {
          max = session[i]->pty;
        }
      }
    }
    int n = select (max + 1, &rfs, NULL, NULL, NULL);
    if (n > 0) {
      if (FD_ISSET (STDIN, &rfs)) {
        int ch = getch ();
        if (ch == 76 || ch == 108) {
          sitas_list_session ();
        }
      }
      for (i = 0; i < MAX_SESSIONS; i++) {
        if (session[i] != NULL && session[i]->usable != true && session[i]->state == CONNECTING) {
          if (FD_ISSET (session[i]->pty, &rfs)) {
            int status = 0;
            pid_t pid = waitpid (session[i]->pid, &status, WNOHANG);
            if (pid == 0) {
              session[i]->state=CONNECTED;
              int flag = fcntl (session[i]->pty, F_GETFL, 0);
              flag |= O_NONBLOCK;
              fcntl (session[i]->pty, F_SETFL, flag);
              /*
              tcgetattr(session[i]->pty, &termwin);
              termwin.c_cc[VMIN] = 0;
              termwin.c_cc[VTIME] = 0;
              tcsetattr(session[i]->pty, TCSANOW, &termwin);
              */
            } else if( pid > 0 ) {
              if (WIFEXITED(status)) {
                session[i]->state=DISCONNECTED;
              } else {
                session[i]->state=CONNECTFAILED;
              }
            }
          }
        }
      }
    }
  }

  int sessions_num = sitas_sessions_num ();

  if (hosts_num != sessions_num){
    werase (termwin);
    mvwprintw (termwin, 1, 0, "%d servers provided, %d servers connected", hosts_num, sessions_num);
    mvwprintw (termwin, 2, 0, "Press L to see status.");
    mvwprintw (termwin, 3, 0, "ESC to quit.");
    mvwprintw (termwin, 4, 0, "ENTER to continue.");
    wrefresh (termwin);
    while (1) {
      int ch = getch ();
      if (ch == 76 || ch == 108) {
        sitas_list_session ();
      } else if (ch == 27) {
        endwin ();
        exit (0);
      }else if (ch == 10) {
        break;
      }
    }
  }
}

int sitas_sessions_num () {
  int sessions_num = 0;
  int i = 0;
  for (i = 0; i < MAX_SESSIONS; i++) {
    if (session[i] != NULL && session[i]->state == CONNECTED){
      sessions_num++; 
    }
  }
  return sessions_num;
}

void sitas_start () {
  raw ();

  char *t = "Help List Single/Multi Quit Add Prev Next Jump";
  mvwprintw (infowin, 1, 0, "sitas v%s", VERSION);
  mvwprintw (infowin, 1, 15, "%s", t);
  mvwchgat (infowin, 1, 0, -1, 0, color_map[COLOR_WHITE][COLOR_BLUE], NULL);
  mvwchgat (infowin, 1, 15, 1, 0, color_map[COLOR_RED][COLOR_BLUE], NULL);
  mvwchgat (infowin, 1, 20, 1, 0, color_map[COLOR_MAGENTA][COLOR_BLUE], NULL);
  mvwchgat (infowin, 1, 25, 1, 0, color_map[COLOR_CYAN][COLOR_BLUE], NULL);
  mvwchgat (infowin, 1, 32, 1, 0, color_map[COLOR_YELLOW][COLOR_BLUE], NULL);
  mvwchgat (infowin, 1, 38, 1, 0, color_map[COLOR_GREEN][COLOR_BLUE], NULL);
  mvwchgat (infowin, 1, 43, 1, 0, color_map[COLOR_RED][COLOR_BLUE], NULL);
  mvwchgat (infowin, 1, 47, 1, 0, color_map[COLOR_CYAN][COLOR_BLUE], NULL);
  mvwchgat (infowin, 1, 52, 1, 0, color_map[COLOR_YELLOW][COLOR_BLUE], NULL);
  mvwchgat (infowin, 1, 57, 1, 0, color_map[COLOR_GREEN][COLOR_BLUE], NULL);

  sitas_visible_session ();
}

void get_new_host (char *buf) {
  sitas_update_info ("Enter host here: ");
  int len = 0;
  while(1) {
    char ch = (char) getch ();
    if (ch == 8) {
      buf[len-1<0?0:len-1]='\0';
      len--;
      sitas_update_info ("Enter host here: %s",buf);
    } else if (ch == '\n') {
      buf[4095]='\0';
      break;
    } else if (ch >= 32 && ch <= 126 && len < 4095) {
      buf[len] = ch;
      len++;
      sitas_update_info ("Enter host here: %s",buf);
    }
  }
}

void sitas_connect () {
  session_init ();
  int i = 0;
  for (i = 0; i < MAX_SESSIONS; i++) {
    if (session[i] != NULL && session[i]->host != NULL) {
      session_new (session[i]);
    }
  }
}

void sitas_update_info (char *fmt, ...) {
  va_list argp;
  va_start (argp, fmt);
  wmove (infowin, 0, 0);
  wclrtoeol (infowin);
  vwprintw (infowin, fmt, argp);
  wrefresh (infowin);
  va_end (argp);
}

void sitas_banner () {
  sitas_update_info ("[ID:%d|S:%d|T:%d] [%s] [%s] %s", current, sitas_sessions_num (), hosts_num, session[current]->host, multi ? "Multi" : "Single", control_mode ? "[control mode]" : "");
  if (control_mode == false) {
    touchwin (session[current]->win);
    wrefresh (session[current]->win);
  }
}

void sitas_forward_key (int ch) {
  if (!multi) {
    session_write2pty (session[current]->pty, ch);
  } else {
    int i = 0;
    for (i = 0; i < MAX_SESSIONS; i++) {
      if (session[i] != NULL && session[i]->pty > 0) {
        session_write2pty (session[i]->pty, ch);
      }
    }
  }
}

void sitas_sigchild_handle (int signum) {
  /*setlock(LOCK); */
  pid_t pid;
  while ((pid = waitpid (-1, NULL, WNOHANG)) > 0) {
    int i = 0;
    for (i = 0; i < MAX_SESSIONS; i++) {
      if (session[i] != NULL && session[i]->pid == pid) {
        session_free (session[i]);
      }
    }
  }
  sitas_visible_session ();
  /*setlock(UNLOCK); */
}

void sitas_visible_session () {
  /* toggle the first available session visible */
  int i=0;
  for (i = 0; i < MAX_SESSIONS; i++) {
    if (session[i] != NULL && session[i]->usable != true && session[i]->state == CONNECTED) {
      current = i;
      sitas_banner ();
      touchwin (session[i]->win);
      wrefresh (session[i]->win);
      break;
    }
  }
}

void sitas_list_session () {
  if (listwin == NULL) {
    listwin = newwin (16, 70, (SCRROWS - 16) / 2, (SCRCOLS - 70) / 2);
    wattrset (listwin, COLOR_PAIR (color_map[COLOR_WHITE][COLOR_BLUE]));
    wattron (listwin, A_BOLD);
  }
  werase (listwin);
  int x, y;
  for (y = 0; y <= 16; y++) {
    for (x = 0; x < 70; x++) {
      waddch (listwin, ' ');
    }
  }
  /* wborder(listwin, 0, 0, 0, 0, 0, 0, 0, 0); */
  mvwprintw (listwin, 0, 1, "Use UP/DOWN arrow keys to scroll, 'q' to quit.");
  y = 1;
  for (x = 0; x < MAX_SESSIONS; x++) {
    if (session[x] != NULL && session[x]->usable != true && y <= 15) {
      int i = session[x]->state;
      mvwprintw (listwin, y++, 1, "id=%d\t  ==  > host = %s\t[%s]", x, session[x]->host, 
          i == NOTCONNECTED ? "NOTCONNECTED" : 
          i == CONNECTING ? "CONNECTING" : 
          i == CONNECTED ? "CONNECTED" :
          i == DISCONNECTED ? "DISCONNECTED" :
          i == CONNECTFAILED ? "CONNECTFAILED":"UNKOWN"
          );
    }
  }
  wrefresh (listwin);
  int cur = 0;
  while (true) {
    x = getch ();
    if (x < 0) {
      continue;
    }
    if (x == KEY_UP) {
      if (cur == 0) {
        continue;
      }
      cur--;
    } else if (x == KEY_DOWN) {
      if (cur >= hosts_num - 15) {
        continue;
      }
      cur++;
    } else if (x == 'q' || x == 'Q') {
      break;
    }
    for (y = 0; y < 16; y++) {
      for (x = 0; x < 70; x++) {
        waddch (listwin, ' ');
      }
    }
    mvwprintw (listwin, 0, 1, "Use UP/DOWN arrow keys to scroll, 'q' to quit.");
    y = 1;
    int t = 0;
    for (x = 0; x < MAX_SESSIONS; x++) {
      if (session[x] != NULL && session[x]->usable != true && y <= 15) {
        if (t++ < cur) {
          continue;
        }
        int i = session[x]->state;
        mvwprintw (listwin, y++, 1, "id=%d\t  ==  > host = %s\t[%s]", x, session[x]->host,
            i == NOTCONNECTED ? "NOTCONNECTED" : 
            i == CONNECTING ? "CONNECTING" : 
            i == CONNECTED ? "CONNECTED" :
            i == DISCONNECTED ? "DISCONNECTED" :
            i == CONNECTFAILED ? "CONNECTFAILED":"UNKOWN"
            );
      }
    }
    wrefresh (listwin);
  }
  touchwin (session[current]->win);
  wrefresh (session[current]->win);
}

void sitas_prev_session () {
  int i = 0;
  bool t = false;
  for (i = current - 1; i >= 0; i--) {
    if (session[i] != NULL && session[i]->usable != true && session[i]->state == CONNECTED) {
      current = i;
      t = true;
      break;
    }
  }
  if (t == false) {
    for (i = MAX_SESSIONS - 1; i >= current; i--) {
      if (session[i] != NULL && session[i]->usable != true && session[i]->state == CONNECTED) {
        current = i;
        break;
      }
    }
  }
  sitas_banner ();
  touchwin (session[current]->win);
  wrefresh (session[current]->win);
}

void sitas_next_session () {
  int i = 0;
  bool t = false;
  for (i = current + 1; i < MAX_SESSIONS; i++) {
    if (session[i] != NULL && session[i]->usable == false && session[i]->state == CONNECTED) {
      current = i;
      t = true;
      break;
    }
  }
  if (t == false) {
    for (i = 0; i < current; i++) {
      if (session[i] != NULL && session[i]->usable == false && session[i]->state == CONNECTED) {
        current = i;
        break;
      }
    }
  }
  sitas_banner ();
  touchwin (session[current]->win);
  wrefresh (session[current]->win);
}

void sitas_jump_session () {
  sitas_update_info ("Enter session id: ");
  char ch;
  char buf[4];
  memset (buf, 0, 4);
  int i = 0;
  while ((ch = (char) wgetch (infowin)) != '\n') {
    if (ch >= '0' && ch <= '9' && i < 3) {
      buf[i++] = ch;
      waddch (infowin, ch);
    }
  }
  if (i == 0) {
    sitas_banner ();
    return;
  }
  i = atoi (buf);
  if (session[i] != NULL && session[i]->usable !=true && session[i]->state == CONNECTED) {
    current = i;
    sitas_banner ();
    touchwin (session[current]->win);
    wrefresh (session[current]->win);
  } else {
    sitas_banner ();
    return;
  }
}

void sitas_add_session () {
  char *buf = malloc (4096);
  memset (buf, 0, 4096);
  get_new_host(buf);

  if (hosts_num == MAX_SESSIONS) {
    sitas_update_info("Max Sessions!");
    return;
  }
  session[hosts_num]->host = buf;
  session[hosts_num]->pid = -1;
  session[hosts_num]->pty = -1;
  session[hosts_num]->win = NULL;
  session[hosts_num]->usable = true;
  session[hosts_num]->state = CONNECTING;
  session[hosts_num]->bold = false;
  session[hosts_num]->underscore = false;
  session[hosts_num]->blink = false;
  session[hosts_num]->reverse = false;
  session[hosts_num]->concealed = false;
  session[hosts_num]->fc = COLOR_WHITE;
  session[hosts_num]->bc = COLOR_BLACK;

  session_new(session[hosts_num]);

  fd_set rfs;
  FD_ZERO (&rfs);
  FD_SET (session[hosts_num]->pty, &rfs);
  int max = session[hosts_num]->pty;
  
  int n = select (max + 1, &rfs, NULL, NULL, NULL);
  if (n > 0) {
    if (FD_ISSET (session[hosts_num]->pty, &rfs)) {
      int status = 0;
      pid_t pid = waitpid (session[hosts_num]->pid, &status, WNOHANG);
      if (pid == 0) {
        session[hosts_num]->state=CONNECTED;
        int flag = fcntl (session[hosts_num]->pty, F_GETFL, 0);
        flag |= O_NONBLOCK;
        fcntl (session[hosts_num]->pty, F_SETFL, flag);
      } else if( pid > 0 ) {
        if (WIFEXITED(status)) {
          session[hosts_num]->state=DISCONNECTED;
        } else {
          session[hosts_num]->state=CONNECTFAILED;
        }
      }
    }
  }
  hosts_num++;
  return;

  int i = 0;
  bool t = false;
  for (i = current + 1; i < MAX_SESSIONS; i++) {
    if (session[i] != NULL && session[i]->usable == false) {
      current = i;
      t = true;
      break;
    }
  }
  if (t == false) {
    for (i = 0; i < current; i++) {
      if (session[i] != NULL && session[i]->usable == false) {
        current = i;
        break;
      }
    }
  }
  sitas_banner ();
  touchwin (session[current]->win);
  wrefresh (session[current]->win);
}

void sitas_switch_mode () {
  multi = !multi;
  sitas_banner ();
  touchwin (session[current]->win);
  wrefresh (session[current]->win);
}

void sitas_help () {
  if (helpwin == NULL) {
    helpwin = newwin (18, 75, (SCRROWS - 17) / 2, (SCRCOLS - 75) / 2);
    werase (helpwin);
    wattrset (helpwin, COLOR_PAIR (color_map[COLOR_WHITE][COLOR_BLUE]));
    wattron (helpwin, A_BOLD);

    int x, y;
    for (y = 0; y < 17; y++) {
      for (x = 0; x < 75; x++) {
        waddch (helpwin, ' ');
      }
    }
    /*
       wborder(helpwin, ACS_VLINE|COLOR_PAIR(color_map[COLOR_WHITE][COLOR_BLUE]), 
       ACS_VLINE|COLOR_PAIR(color_map[COLOR_WHITE][COLOR_BLUE]), 
       ACS_HLINE|COLOR_PAIR(color_map[COLOR_WHITE][COLOR_BLUE]), 
       ACS_HLINE|COLOR_PAIR(color_map[COLOR_WHITE][COLOR_BLUE]), 
       ACS_ULCORNER|COLOR_PAIR(color_map[COLOR_WHITE][COLOR_BLUE]), 
       ACS_URCORNER|COLOR_PAIR(color_map[COLOR_WHITE][COLOR_BLUE]), 
       ACS_LLCORNER|COLOR_PAIR(color_map[COLOR_WHITE][COLOR_BLUE]), 
       ACS_LRCORNER|COLOR_PAIR(color_map[COLOR_WHITE][COLOR_BLUE]));
       */
    mvwaddstr (helpwin, 0, 34, "HELP");
    mvwaddstr (helpwin, 1, 1,  "Sitas -- Send Input To All SSH Sessions");
    mvwaddstr (helpwin, 2, 1,  "Sitas allows you to login several servers simultaneously, and forward");
    mvwaddstr (helpwin, 3, 1,  "every key you type to all servers. Meanwhile sitas can switch into");
    mvwaddstr (helpwin, 4, 1,  "singlecast mode and every key you type will be sent to current session");
    mvwaddstr (helpwin, 5, 1,  "only. Sitas can handle a subset of the more sophisticated ANSI escape");
    mvwaddstr (helpwin, 6, 1,  "code well,  which means graphical programs like VIM are also supported");
    mvwaddstr (helpwin, 7, 1,  "Press ctrl-f to enter control mode first");
    mvwaddstr (helpwin, 8, 1,  "H: Show this help menu.");
    mvwaddstr (helpwin, 9, 1,  "L: List all active sessions by ID => HOST pairs.");
    mvwaddstr (helpwin, 10, 1, "S/M: Switch between SingleCast and MultiCast mode.");
    mvwaddstr (helpwin, 11, 1, "Q: Quit.");
    mvwaddstr (helpwin, 12, 1, "A: Add new session.");
    mvwaddstr (helpwin, 13, 1, "P: Move to the prev session and toggle it current active.");
    mvwaddstr (helpwin, 14, 1, "N: Move to the next session and toggle it current active.");
    mvwaddstr (helpwin, 15, 1, "J: Jump to the session by ID and toggle it current active.");
    mvwaddstr (helpwin, 16, 1, "Any key to quit this.");
    wattroff (helpwin, COLOR_PAIR (color_map[COLOR_WHITE][COLOR_GREEN]));
  }
  touchwin (helpwin);
  wrefresh (helpwin);
  getch ();
  touchwin (session[current]->win);
  wrefresh (session[current]->win);
}

void sitas_quit () {
  sitas_update_info ("Do you really want to quit?[Y]");
  char ch = (char) getch ();
  if (ch == '\n' || ch == 'y' || ch == 'Y') {
    int i = 0;
    for (i = 0; i < MAX_SESSIONS; i++) {
      if (session[i] != NULL && session[i]->pid > 0) {
        kill (session[i]->pid, SIGUSR2);
      }
    }
    endwin ();
    exit (0);
  }
  sitas_banner ();
}


void session_new (Session * s) {
  int pty;
  pid_t pid;
  struct winsize ws;
  pid_t childpid;

  ws.ws_row = PTYROWS;
  ws.ws_col = PTYCOLS;
  ws.ws_xpixel = ws.ws_ypixel = 0;

  /*
     setenv("TERM", "xterm", 1);
     setenv("TERM", "ansi", 1);
     */
  setenv ("TERM", "linux", 1);

  pid = forkpty (&pty, NULL, NULL, &ws);
  if (pid < 0) {
    s->usable = true;
    return;
  } else if (pid == 0) {
    execl ("/usr/bin/ssh", "ssh", s->host, NULL);
    exit (127);
  } else if (pid > 0) {
    s->pid = pid;
    s->pty = pty;
  }
  s->usable = false;
  s->state = CONNECTING;
  s->win = newwin (TERMROWS, TERMCOLS, SCRROWS - TERMROWS, 0);
  scrollok (s->win, true);
}

void session_free (Session * session) {
  // just mark state to NOTCONNECTED
  if (session != NULL) {
    session->state = NOTCONNECTED;
  }
  return;

  if (session != NULL) {
    session->usable = true;
    free (session->host);
    session->host = NULL;
    session->pid = -1;
    close (session->pty);
    session->pty = -1;
    session->state = NOTCONNECTED;
    delwin (session->win);
    session->win = NULL;
    hosts_num--;
  }
}

void sitas_child_event (int i) {
  if (session[i] != NULL && session[i]->usable != true && session[i]->state == CONNECTED) {
    fd_set rfs;
    FD_ZERO (&rfs);
    FD_SET (session[i]->pty, &rfs);
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    char buf[MAX_BUFFER];
    memset (buf, 0, MAX_BUFFER);
    if ((select (session[i]->pty + 1, &rfs, NULL, NULL, &tv) > 0)) {
      if (FD_ISSET (session[i]->pty, &rfs)) {
        int n = read (session[i]->pty, buf, MAX_BUFFER);
        if (n <= 0 && errno == EIO) {
          session_free (session[i]);
          sitas_next_session ();
        } else if (n <= 0 && errno == EAGAIN) {

        } else {
          session_print (session[i], buf, n);
          if (i == current) {
            wrefresh (session[current]->win);
          }
        }
      }
    }
  }
}

/*
void update (int signum, siginfo_t * info, void *a) {
  int n;
  char buf[MAX_BUFFER];
  memset (buf, 0, MAX_BUFFER);
  Session *s;
  if (signum == SIG_UPDATE && info) {
    s = (Session *) (info->si_value.sival_ptr);
    fd_set rfs;
    FD_ZERO (&rfs);
    FD_SET (s->pty, &rfs);
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    if ((select (s->pty + 1, &rfs, NULL, NULL, &tv) > 0)) {
      if (FD_ISSET (s->pty, &rfs)) {
        n = read (s->pty, buf, MAX_BUFFER);
        if (n <= 0 && errno == EIO) {
          session_free (s);
          sitas_next_session ();
        } else if (n <= 0 && errno == EAGAIN) {

        } else {
          session_print (s, buf, n);
          if (s == session[current]) {
            wrefresh (session[current]->win);
          }
          //sitas_help(); 
        }
      }
    }
    //kill(info->si_pid, SIG_CON); 
  }
}
*/

void session_print (Session * s, char *buf, int n) {
  int i = 0;
  int x, y;
  for (i = 0; i < n; i++) {
    if (s->escape) {
      session_escape_attr (s, buf[i]);
    } else {
      if (buf[i] == '\r') {
        getyx (s->win, y, x);
        wmove (s->win, y, 0);
      } else if (buf[i] == '\n') {
        getyx (s->win, y, x);
        wmove (s->win, y, PTYCOLS);
        waddch (s->win, '\n');
      } else if (buf[i] == '\t') {
        getyx (s->win, y, x);
        wmove (s->win, y, x + 8);
      } else if (buf[i] == '\b') {
        getyx (s->win, y, x);
        wmove (s->win, y, x - 1 < 0 ? 0 : x - 1);
      } else if (buf[i] == '\x1b') {			/* CSI:ESC [ */
        s->escape = true;
        s->escape_len = 0;
        memset (s->escape_buf, 0, 128);
        s->escape_buf[s->escape_len++] = buf[i];
      } else if (buf[i] == '\x9b') {			/* single char CSI */
        s->escape = true;
        s->escape_len = 0;
        memset (s->escape_buf, 0, 128);
        s->escape_buf[s->escape_len++] = buf[i];
        s->escape_buf[s->escape_len++] = '[';
      } else if (buf[i] >= 0 && buf[i] < 32) {

      } else {
        /* }else if(buf[i] >=  32){ */
        unsigned t = buf[i];
        if (buf[i] < 0) {
          t = t & 0x000000ff;
        } if (insmod) {
          winsch (s->win, t);
          getyx (s->win, y, x);
          wmove (s->win, y, x + 1);
          if ((--insnum) == 0) {
            insmod = false;
          }
        } else {
          waddch (s->win, t);
        }
      }
    }
  }
}

void session_escape_attr (Session * s, char c) {
  s->escape_buf[s->escape_len++] = c;
  if (!  (c == '@' || (c >= 'A' && c <= 'Z') || c == '`' || (c >= 'a' && c <= 'z'))) {
    return;
  }

  int attr[MAX_ESCAPE_ARGC];
  int x, y;
  getyx (s->win, y, x);

  if (s->escape_len == 2) {
    if (c == 'M') {
      if (y == 0) {
        wscrl (s->win, -1);
      } else {
        wmove (s->win, y - 1, x);
      }
    }
    s->escape = false;
    return;
  } else {
    parse_escape (s->escape_buf, attr);
  }
  s->escape = false;

  int i = 0;
  switch (c) {
    case 'A': /*CUU*/ 
      if (attr[0] == -1) {
        attr[0] = 1;
      }
      wmove (s->win, y - attr[0] > 0 ? y - attr[0] : 0, x);
      break;
    case 'B': /*CUD*/ 
    case 'e': /*VPR*/ 
      if (attr[0] == -1) {
        attr[0] = 1;
      }
      wmove (s->win, y + attr[0] < PTYROWS ? y + attr[0] : PTYROWS, x);
      break;
    case 'C': /*CUF*/ 
    case 'a': /*HPR*/ 
      if (attr[0] == -1) {
        attr[0] = 1;
      }
      wmove (s->win, y, x + attr[0] < PTYCOLS ? x + attr[0] : PTYCOLS);
      break;
    case 'D': /*CUB*/ 
      if (attr[0] == -1) {
        attr[0] = 1;
      }
      wmove (s->win, y, x - attr[0] > 0 ? x - attr[0] : 0);
      break;
    case 'd': /*VPA*/ 
      if (attr[0] == -1) {
        attr[0] = 0;
      }
      wmove (s->win, attr[0] > 0 ? attr[0] : 0, x);
      break;
    case 'E': /*NEL*/ 
      if (attr[0] == -1) {
        attr[0] = PTYROWS;
      }
      wmove (s->win, y + attr[0] < PTYROWS ? y + attr[0] : PTYROWS, 0);
      break;
    case 'F': /*CPL*/ 
      if (attr[0] == -1) {
        attr[0] = y;
      }
      wmove (s->win, y - attr[0] > 0 ? y - attr[0] : 0, 0);
      break;
    case 'G': /*CHA*/ 
    case '`': /*HPA*/ 
      if (attr[0] == -1) {
        attr[0] = 0;
      }
      wmove (s->win, y, attr[0] > 0 ? attr[0] : 0);
      break;
    case 'H': /*CUP*/ 
    case 'f': /*HVP*/ 
      if (attr[0] == -1) {
        attr[0] = 1;
      }
      if (attr[1] == -1) {
        attr[1] = 1;
      }
      int my = attr[0] - 1;
      int mx = attr[1] - 1;
      if (my < 0) {
        my = 0;
      } else if (my > PTYROWS - 1) {
        my = PTYROWS - 1;
      }
      if (mx < 0) {
        mx = 0;
      }
      if (mx > PTYCOLS) {
        mx = PTYCOLS - 1;
      }
      wmove (s->win, my, mx);
      break;
    case 'J': /*ED*/ 
      if (attr[0] == -1 || attr[0] == 0) {
        wclrtobot (s->win);
      } else if (attr[0] == 1) {			/* from beginning through cursor */

      } else if (attr[0] == 2) {
        werase (s->win);
      }
      break;
    case 'K': /*EL*/ 
      if (attr[0] == -1 || attr[0] == 0) {
        wclrtoeol (s->win);
      } else if (attr[0] == 1) {			/* from beginning through cursor */

      }
      else if (attr[0] == 2) {
        wmove (s->win, y, 0);
        wclrtoeol (s->win);
      }
      break;
    case 'L': /*IL*/ 
      if (attr[0] == -1) {
        attr[0] = 1;
      }
      wscrl (s->win, -attr[0]);
      break;
    case 'M': /*DL*/ 
      if (attr[0] == -1) {
        attr[0] = 1;
      }
      wscrl (s->win, attr[0]);
      break;
    case 'm': /*SGR*/
      {
        bool underscore = s->underscore;
        bool bold = s->bold;
        bool blink = s->blink;
        bool reverse = s->reverse;
        bool concealed = s->concealed;
        int fc = s->fc;
        int bc = s->bc;
        bool normal = true;
        for (i = 0; i < MAX_ESCAPE_ARGC; i++) {
          if (attr[i] == -1) {
            continue;
          }
          normal = false;
          if (attr[i] == 0) {
            bold = underscore = blink = reverse = concealed = false;
            fc = COLOR_WHITE, bc = COLOR_BLACK;
            wattrset (s->win, A_NORMAL);
          } else if (attr[i] == 1) {
            bold = true;
          } else if (attr[i] == 4) {
            underscore = true;
          } else if (attr[i] == 5) {
            blink = true;
          } else if (attr[i] == 7) {
            reverse = true;
          } else if (attr[i] == 27) {
            reverse = false;
          } else if (attr[i] == 8) {
            concealed = true;	/* not implement */
          } else if (attr[i] == 22) {
            bold = false;
          } else if (attr[i] == 24) {
            underscore = false;
          } else if (attr[i] == 25) {
            blink = false;
          } else if (attr[i] == 28) {
            concealed = false;
          } else if (attr[i] >= 30 && attr[i] <= 37) {
            fc = attr[i] - 30;
          } else if (attr[i] == 39) {
            fc = COLOR_WHITE;
          } else if (attr[i] >= 40 && attr[i] <= 47) {
            bc = attr[i] - 40;
          } else if (attr[i] == 49) {
            bc = COLOR_BLACK;
          }
        }
        if (normal == true) {
          bold = underscore = blink = reverse = concealed = false;
          fc = COLOR_WHITE, bc = COLOR_BLACK;
          wattrset (s->win, A_NORMAL);
        } else {
          if (reverse) {
            wattron (s->win, COLOR_PAIR (color_map[bc][fc]));
          } else {
            wattron (s->win, COLOR_PAIR (color_map[fc][bc]));
          }
          if (bold) {
            wattron (s->win, A_BOLD);
          }
          else {
            wattroff (s->win, A_BOLD);
          }
          if (underscore) {
            wattron (s->win, A_UNDERLINE);
          } else {
            wattroff (s->win, A_UNDERLINE);
          }
          if (blink) {
            wattron (s->win, A_BLINK);
          } else {
            wattroff (s->win, A_BLINK);
          }
        }
        s->bold = bold;
        s->underscore = underscore;
        s->blink = blink;
        s->reverse = reverse;
        s->concealed = concealed;
        s->fc = fc;
        s->bc = bc;
      }
      break;
    case '@': /*ICH*/ 
      if (attr[0] == -1) {
        attr[0] = 1;
      }
      insmod = true;
      insnum = attr[0];
      break;
    case 'P': /*DCH*/ 
      if (attr[0] == -1) {
        attr[0] = 1;
      }
      int t = 0;
      for (; t < attr[0]; t++) {
        wdelch (s->win);
      }
      break;
    case 'r': /*DECSTBM*/ 
      if (attr[0] == -1) {
        attr[0] = 1;
      }
      if (attr[1] == -1) {
        attr[1] = PTYROWS;
      }
      int top = attr[0] - 1;
      int bot = attr[1] - 1;
      if (top < 0) {
        top = 0;
      }
      if (top >= PTYROWS) {
        top = PTYROWS - 1;
      }
      if (bot < 0) {
        bot = 0;
      }
      if (bot >= PTYROWS) {
        bot = PTYROWS - 1;
      }
      if (top > bot) {
        return;
      }
      wsetscrreg (s->win, top, bot);
      break;
    case 's': /*SCORC*/ 
      if (s->escape_len == 3) {
        getyx (s->win, y, x);
        s->sy = y, s->sx = x;
      }
      break;
    case 'u': /*SCORC*/ 
      if (s->escape_len == 3) {
        wmove (s->win, s->sy, s->sx);
      }
      break;
  }
}

void parse_escape (char *buf, int *attr) {
  memset (attr, -1, MAX_ESCAPE_ARGC * 4);
  if (strlen (buf) == 3 || buf[2] == '?') {
    return;
  }
  char cp[128];
  memset (cp, 0, 128);
  memcpy (cp, buf + 2, strlen (buf) - 3);
  char *p = strtok (cp, ";");
  int i = 0;
  if (p) {
    attr[i++] = atoi (p);
  } else {
    return;
  }
  while (1) {
    p = strtok (NULL, ";");
    if (p) {
      attr[i++] = atoi (p);
    } else {
      return;
    }
  }
}

void session_write2pty (int pty, int ch) {
  char c = (char) ch;
  if (ch >= 0 && ch < KEY_MAX && skey[ch]) {
    Write (pty, skey[ch], strlen (skey[ch]));
  } else {
    Write (pty, &c, 1);
  }
}

void Write (int pty, const char *data, int len) {
  while (len > 0) {
    int n = write (pty, data, len);
    if (n < 0 && errno == EIO) {
      return;			/* session_free */
    }
    if (n < 0) {
      continue;
    }
    data += n;
    len -= n;
  }
}

static void clear_prev_key() {
  prev_key=-1;
  sitas_banner ();
  control_mode = false;
}

char * trim_right (char *str) {
  char *s = str;
  int len = strlen (s);
  int i = 0;
  for (i = len - 1; i >= 0; i--) {
    if (s[i] == ' ' || s[i] == '\t' || s[i] == '\n') {
      s[i] = '\0';
    } else {
      break;
    }
  }
  return str;
}

int setlock (int cmd) {
  if (cmd == LOCK) {
    lock->l_type = F_WRLCK;
  } else if (cmd == UNLOCK) {
    lock->l_type = F_UNLCK;
  }

  lock->l_whence = SEEK_SET;
  lock->l_start = lock->l_len = 0;
  return fcntl (lockfd, F_SETLKW, lock);
}

pid_t forkpty (int *amaster, char *name, struct termios * termp, struct winsize * winp) {
  int master, slave, pid;
  char *slavename;

  master = posix_openpt (O_RDWR | O_NOCTTY);
  /* master = open("/dev/ptmx", O_RDWR|O_NOCTTY); */
  if (master == -1) {
    close (master);
    return (-1);
  }
  if (grantpt (master) == -1) {
    close (master);
    return (-1);
  }
  if (unlockpt (master) == -1) {
    close (master);
    return (-1);
  }
  slavename = ptsname (master);
  if (slavename == NULL) {
    close (master);
    return (-1);
  }
  slave = open (slavename, O_RDWR | O_NONBLOCK);
  if (slave == -1) {
    close (master);
    return (-1);
  }

  if (name) {
    strcpy (name, slavename);
  }
  if (termp) {
    tcsetattr (slave, TCSAFLUSH, termp);
  }
  if (winp) {
    ioctl (slave, TIOCSWINSZ, (char *) winp);
  }

  pid = fork ();
  if (pid == -1) {
    return (-1);
  } else if (pid == 0) {
    close (master);
    setsid ();
    if (ioctl (slave, TIOCSCTTY, (char *) NULL) == -1) {
      return (-1);
    }
    dup2 (slave, STDIN);
    dup2 (slave, STDOUT);
    dup2 (slave, STDERR);
    if (slave > 2) {
      close (slave);
    }
    return (0);
  } else {
    *amaster = master;
    close (slave);
    return (pid);
  }
}

void help () {
  printf ("sitas: Send Input To All SSH Sessions\n");
  printf ("Usage: sitas -f hostfile [-m integer] [-hv]\n");
  printf ("\t-f hostfile\tread hosts/ips from hostfile and connect\n");
  printf ("\t-m integer\tset max sessions to `integer`, default: %d\n", MAX_SESSIONS);
  printf ("\t-v\t\toutput version information and exit\n");
  printf ("\t-h\t\tdisplay this help and exit\n");
}


void sitas_signal () {
  signal (SIGCHLD, sitas_sigchild_handle);
  signal (SIGALRM, clear_prev_key);
}

int main (int argc, char **argv) {

  parse_cmdline (argc, argv);

  sitas_init ();
  sitas_connect ();
  sitas_splash ();
  sitas_start ();

  sitas_main_loop ();

  endwin();
  return 0;
}

void sitas_main_loop () {
  while (1) {
    sitas_banner ();
    bool live = false;
    int i = 0;
    fd_set rfs;
    FD_ZERO (&rfs);
    FD_SET (STDIN, &rfs);
    int max = STDIN;
    for (i = 0; i < MAX_SESSIONS; i++) {
      if (session[i] != NULL && session[i]->usable != true && session[i]->state == CONNECTED) { 
        live = true;
        FD_SET (session[i]->pty, &rfs);
        if (max < session[i]->pty) {
          max = session[i]->pty;
        }
      }
    }

    if ( live == false ) {
      break;
    }

    int n = select (max + 1, &rfs, NULL, NULL, NULL);
    if (n > 0) {
      if (FD_ISSET (STDIN, &rfs)) {
        sitas_user_event ();
      }
      for (i = 0; i < MAX_SESSIONS; i++) {
        if (session[i] != NULL && session[i]->usable != true && session[i]->state == CONNECTED) {
          if (FD_ISSET (session[i]->pty, &rfs)) {
            sitas_child_event(i);
          }
        }
      }
    }
  }
  /*
    n = select (max + 1, &rfs, NULL, NULL, NULL);
    if (n > 0) {
      for (i = 0; i < MAX_SESSIONS; i++) {
        if (session[i] != NULL && session[i]->usable != true) {
          if (FD_ISSET (session[i]->pty, &rfs)) {
            union sigval val;
            val.sival_ptr = (void *) session[i];
            sigqueue (getppid (), SIG_UPDATE, val);
            while (sigflag == 0) {
              sigsuspend (&zeromask);
            }
            sigflag = 0;
            sigprocmask (SIG_BLOCK, &newmask, &oldmask);
            //pause();
          }
        }
      }
    }
   */
}

void sitas_user_event () {
  int ch = getch ();
  if (ch < 0) {
    return;
  }
  if (ch == 6 && prev_key == -1) { /* ctrl-f */
    prev_key=ch;
    control_mode = true;
    alarm(1);
    return;
  }
  fflush (wfile);
  if (prev_key == 6) {
    switch (ch) {
      case 6:
        sitas_forward_key (ch);
        control_mode = false;
        break;
        //      case KEY_F (1):
      case 'h':
      case 'H':
        sitas_help ();
        break;
      case 'l':
      case 'L':
        sitas_list_session ();
        break;
      case 's':
      case 'S':
      case 'm':
      case 'M':
        sitas_switch_mode ();
        break;
      case 'q':
      case 'Q':
        alarm(0);
        sitas_quit ();
        break;
      case 'p':
      case 'P':
        sitas_prev_session ();
        break;
      case 'n':
      case 'N':
        sitas_next_session ();
        break;
      case 'j':
      case 'J':
        alarm(0);
        sitas_jump_session ();
        break;
      case 'a':
      case 'A':
        alarm(0);
        sitas_add_session ();
        break;
      default:
        break;
    }
    control_mode = false;
    clear_prev_key();
  } else {
    sitas_forward_key (ch);
  }

}

