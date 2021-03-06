# Sitas
> Send Input To All SSH Sessions

Sitas allows you to login several servers simultaneously, and forwards every key you typed to all sessions. Meanwhile sitas can switch into singlecast mode and every key you typed will be sent to current session only. Sitas can handle a subset of the more sophisticated ANSI escape code well, which means graphical applications like VIM are also supported.

## Installation

    $ make

## Usage

Create a file:

```bash
user@host1
user@1.2.3.4
user@host2:2222
```

    $ ./sitas -f file

![01](https://cloud.githubusercontent.com/assets/19603306/15599032/f2a5f5aa-241a-11e6-9262-4527c5b46e22.png)

### Control mode

Use `CTRL + f` to enter control mode, after that, you can use these keys:

```
H: Show this help menu.                                                   
L: List all active sessions by ID => HOST pairs.                          
S/M: Switch between SingleCast and MultiCast mode.
Q: Quit.                                                                  
A: Add new session.           
P: Move to the prev session and toggle it current active.                 
N: Move to the next session and toggle it current active.
J: Jump to the session by ID and toggle it current active. 
```

![02](https://cloud.githubusercontent.com/assets/19603306/15599020/dd16953c-241a-11e6-80d9-77381e413049.png)
