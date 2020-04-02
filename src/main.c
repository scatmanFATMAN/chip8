#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <time.h>
#include <ncurses.h>
#include <unistd.h>
#include <pthread.h>

#define GFX_WIDTH  64
#define GFX_HEIGHT 32

#define LOG_LINES_MAX 18
#define LOG_LINE_LEN  GFX_WIDTH

#define DEBUGGER_LINES_MAX ((GFX_HEIGHT) + 1 + 1 + (LOG_LINES_MAX))
#define DEBUGGER_LINE_LEN  50

//game window
static WINDOW *win_game;

//log window
static WINDOW *win_log;

//debugging window
static WINDOW *win_debugger;

//thread to count down the delay timer and sound timer at 60Hz
static pthread_t thread_timers;

//thread to capture the keyboard
static pthread_t thread_keys;

//index register
static uint16_t I;

//program counter
static uint16_t pc;

//current opcode being processed
static uint16_t opcode;

//stack pointer
static uint8_t sp;

//delay timer and its lock
static uint8_t dt;
static pthread_rwlock_t dt_lock;

//sound timer and its lock
static uint8_t st;
static pthread_rwlock_t st_lock;

static unsigned char memory[4096];

//15 CPU registers, with the 16th one used for the carry flag
static unsigned char V[16];

static uint16_t stack[16];

//represents what's currently being displayed
static unsigned char gfx[GFX_WIDTH * GFX_HEIGHT];

//currently pressed keys
static unsigned char key[16];

//maps to
// Keypad
// +-+-+-+-+
// |1|2|3|C|
// +-+-+-+-+
// |4|5|6|D|
// +-+-+-+-+
// |7|8|9|E|
// +-+-+-+-+
// |A|0|B|F|
// +-+-+-+-+
static unsigned char key_map[16] = {
    '1', '2', '3', '4',
    'q', 'w', 'e', 'r',
    'a', 's', 'd', 'f',
    'z', 'x', 'c', 'v'
};

static unsigned char font_set[] = {
    0xF0, 0x90, 0x90, 0x90, 0xF0, //0
    0x20, 0x60, 0x20, 0x20, 0x70, //1
    0xF0, 0x10, 0xF0, 0x80, 0xF0, //2
    0xF0, 0x10, 0xF0, 0x10, 0xF0, //3
    0x90, 0x90, 0xF0, 0x10, 0x10, //4
    0xF0, 0x80, 0xF0, 0x10, 0xF0, //5
    0xF0, 0x80, 0xF0, 0x90, 0xF0, //6
    0xF0, 0x10, 0x20, 0x40, 0x40, //7
    0xF0, 0x90, 0xF0, 0x90, 0xF0, //8
    0xF0, 0x90, 0xF0, 0x10, 0xF0, //9
    0xF0, 0x90, 0xF0, 0x90, 0x90, //A
    0xE0, 0x90, 0xE0, 0x90, 0xE0, //B
    0xF0, 0x80, 0x80, 0x80, 0xF0, //C
    0xE0, 0x90, 0x90, 0x90, 0xE0, //D
    0xF0, 0x80, 0xF0, 0x80, 0xF0, //E
    0xF0, 0x80, 0xF0, 0x80, 0x80  //F
};

static bool draw_game;
static bool draw_log;

static char log_lines[LOG_LINES_MAX][LOG_LINE_LEN + 1];

static bool looping = true;
static bool debugger_stepping = false;

//frames per second
static const char *opt_path = NULL;
static int opt_fps = 120;
static int opt_color = COLOR_GREEN;

static uint64_t counter_frames;
static time_t program_start;

static uint64_t
time_ms() {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);

    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void
log_write(const char *fmt, ...) {
    va_list ap;
    int i;

    //shift lines up to make room for our new line
    for (i = LOG_LINES_MAX - 1; i > 0; i--) {
        memcpy(log_lines[i], log_lines[i - 1], LOG_LINE_LEN + 1);
    }

    va_start(ap, fmt);
    vsnprintf(log_lines[0], LOG_LINE_LEN + 1, fmt, ap);
    va_end(ap);

    draw_log = true;
}

static void
initialize() {
    memset(memory, 0, sizeof(memory));
    memset(V, 0, sizeof(V));
    memset(stack, 0, sizeof(stack));
    memset(gfx, 0, sizeof(gfx));
    memset(key, 0, sizeof(key));

    //program counter starts 512 bytes into memory
    pc = 0x200;

    opcode = 0;
    I = 0;
    sp = 0;
    dt = 0;
    st = 0;
    draw_game = false;
    draw_log = false;

    //load the font set into memory
    memcpy(memory, font_set, sizeof(font_set));

    win_game = newwin(GFX_HEIGHT + 2, GFX_WIDTH + 2, 0, 0);
    win_log = newwin(LOG_LINES_MAX + 2, LOG_LINE_LEN + 2, GFX_HEIGHT + 2, 0);
    win_debugger = newwin(DEBUGGER_LINES_MAX + 2, DEBUGGER_LINE_LEN + 2, 0, GFX_WIDTH + 2);

    //refresh the stdscr now, since we'll be reading input from it and getch() will cause a refresh() if we don't do it here
    wrefresh(stdscr);

    box(win_game, ACS_VLINE, ACS_HLINE);
    box(win_log, ACS_VLINE, ACS_HLINE);
    box(win_debugger, ACS_VLINE, ACS_HLINE);

    wrefresh(win_game);
    wrefresh(win_log);
    wrefresh(win_debugger);

    nodelay(stdscr, TRUE);
    keypad(stdscr, TRUE);

    nodelay(win_game, TRUE);
    keypad(win_game, TRUE);

    keypad(win_debugger, TRUE);

    memset(log_lines, 0, sizeof(log_lines));

    pthread_rwlock_init(&dt_lock, NULL);
    pthread_rwlock_init(&st_lock, NULL);
}

static bool
load() {
    FILE *f;
    size_t count;

    log_write("Loading %s", opt_path);

    f = fopen(opt_path, "rb");
    if (f == NULL) {
        log_write("%s", strerror(errno));
        return false;
    }

    //read the ROM starting at 512 bytes into memory
    count = fread(memory + 512, sizeof(unsigned char), sizeof(memory) - 512, f);
    fclose(f);

    if (count < sizeof(opcode)) {
        log_write("Invalid ROM");
        return false;
    }

    return true;
}

static void
draw_game_win() {
    int x, y;

    for (y = 0; y < GFX_HEIGHT; y++) {
        for (x = 0; x < GFX_WIDTH; x++) {
            wmove(win_game, y + 1, x + 1);

            if (gfx[x + (y * 64)]) {
                wattron(win_game, A_REVERSE | COLOR_PAIR(1));
                waddch(win_game, ' ');
                wattroff(win_game, A_REVERSE | COLOR_PAIR(1));
            }
            else {
                waddch(win_game, ' ');
            }
        }
    }

    wrefresh(win_game);
}

static void
draw_log_win() {
    int i, j, c;
    bool done;

    //we need to touch every coordinate in the window
    //if we're done drawing the string (reached NULL), then we need to fill
    //the remainder of the line with blank spaces so we overwrite any previous
    //and longer string that might have been there
    for (i = 0; i < LOG_LINES_MAX; i++) {
        done = false;

        for (j = 0; j < LOG_LINE_LEN; j++) {
            if (done) {
                c = ' ';
            }
            else {
                if (log_lines[i][j] == '\0') {
                    done = true;
                    c = ' ';
                }
                else {
                    c = log_lines[i][j];
                }
            }

            mvwaddch(win_log, LOG_LINES_MAX - i, j + 1, c);
        }
    }

    wrefresh(win_log);
}

static void
draw_debugger_win(const char *state) {
    int row, col, c;
    time_t t;

    mvwprintw(win_debugger, 1, 1, "State: %s", state);

    mvwprintw(win_debugger, 3, 1, "Memory");
    mvwprintw(win_debugger, 4, 1, "PC: %u           I: %u", pc, I);
    mvwprintw(win_debugger, 5, 1, "Opcode: %04X", opcode);
    for (col = 1; col < DEBUGGER_LINE_LEN - 1; col++) {
        mvwaddch(win_debugger, 6, col, ' ');
    }

    //TODO: add the rest of the opcodes
    switch (opcode & 0xF000) {
        case 0x2000:
            mvwprintw(win_debugger, 6, 1, "Call subroutine at NNN");
            break;
        case 0x3000:
            mvwprintw(win_debugger, 6, 1, "Skip next if VX == NN");
            break;
        case 0xF000:
            switch (opcode & 0x00FF) {
                case 0x0055:
                    mvwprintw(win_debugger, 6, 1, "Copy {V0,VX} to {memory[I],memory[I + X]}");
                    break;
                case 0x0065:
                    mvwprintw(win_debugger, 6, 1, "Copy {memory[I],memory[I + X]} to {V0,VX}");
                    break;
            }

            break;
    }


    row = 8;
    mvwprintw(win_debugger, row, 1, "Registers");
    mvwprintw(win_debugger, row + 1, 1, "V0: 0x%02X", V[0]);
    mvwprintw(win_debugger, row + 1, 15, "V1: 0x%02X", V[1]);
    mvwprintw(win_debugger, row + 1, 30, "V2: 0x%02X", V[2]);
    mvwprintw(win_debugger, row + 2, 1, "V3: 0x%02X", V[3]);
    mvwprintw(win_debugger, row + 2, 15, "V4: 0x%02X", V[4]);
    mvwprintw(win_debugger, row + 2, 30, "V5: 0x%02X", V[5]);
    mvwprintw(win_debugger, row + 3, 1, "V6: 0x%02X", V[6]);
    mvwprintw(win_debugger, row + 3, 15, "V7: 0x%02X", V[7]);
    mvwprintw(win_debugger, row + 3, 30, "V8: 0x%02X", V[8]);
    mvwprintw(win_debugger, row + 4, 1, "V9: 0x%02X", V[9]);
    mvwprintw(win_debugger, row + 4, 15, "VA: 0x%02X", V[10]);
    mvwprintw(win_debugger, row + 4, 30, "VB: 0x%02X", V[11]);
    mvwprintw(win_debugger, row + 5, 1, "VC: 0x%02X", V[12]);
    mvwprintw(win_debugger, row + 5, 15, "VD: 0x%02X", V[13]);
    mvwprintw(win_debugger, row + 5, 30, "VE: 0x%02X", V[14]);
    mvwprintw(win_debugger, row + 6, 1, "VF: 0x%02X", V[15]);

    row += 8;
    mvwprintw(win_debugger, row, 1, "Stack");
    mvwprintw(win_debugger, row + 1, 1, "SP: %u", sp);
    mvwprintw(win_debugger, row + 2, 1, "S0: 0x%04X", stack[0]);
    mvwprintw(win_debugger, row + 2, 15, "S1: 0x%04X", stack[1]);
    mvwprintw(win_debugger, row + 2, 30, "S2: 0x%04X", stack[2]);
    mvwprintw(win_debugger, row + 3, 1, "S3: 0x%04X", stack[3]);
    mvwprintw(win_debugger, row + 3, 15, "S4: 0x%04X", stack[4]);
    mvwprintw(win_debugger, row + 3, 30, "S5: 0x%04X", stack[5]);
    mvwprintw(win_debugger, row + 4, 1, "S6: 0x%04X", stack[6]);
    mvwprintw(win_debugger, row + 4, 15, "S7: 0x%04X", stack[7]);
    mvwprintw(win_debugger, row + 4, 30, "S8: 0x%04X", stack[8]);
    mvwprintw(win_debugger, row + 5, 1, "S9: 0x%04X", stack[9]);
    mvwprintw(win_debugger, row + 5, 15, "SA: 0x%04X", stack[10]);
    mvwprintw(win_debugger, row + 5, 30, "SB: 0x%04X", stack[11]);
    mvwprintw(win_debugger, row + 6, 1, "SC: 0x%04X", stack[12]);
    mvwprintw(win_debugger, row + 6, 15, "SD: 0x%04X", stack[13]);
    mvwprintw(win_debugger, row + 6, 30, "SE: 0x%04X", stack[14]);
    mvwprintw(win_debugger, row + 7, 1, "SF: 0x%04X", stack[15]);

    row += 9;
    pthread_rwlock_rdlock(&dt_lock);
    mvwprintw(win_debugger, row, 1, "DT: %u", dt);
    pthread_rwlock_unlock(&dt_lock);
    mvwprintw(win_debugger, row + 1, 1, "ST: %u", st);

    row += 3;
    t = time(NULL) - program_start;
    mvwprintw(win_debugger, row, 1, "Target FPS: %d", opt_fps);
    if (t > 0) {
        mvwprintw(win_debugger, row + 1, 1, "Actual FPS: %ld", counter_frames / t);
    }

    wrefresh(win_debugger);

    if (debugger_stepping) {
        mvwprintw(win_debugger, row + 3, 1, "Press enter to step."); 
        while (true) {
            c = wgetch(win_debugger);

            if (c == 10) {
                break;
            }
        }
    }
}

static bool
cycle() {
    uint16_t x, xx, y, yy, height, pixel;
    bool press;
    int i;

    opcode = memory[pc] << 8 | memory[pc + 1];

    //temporary: was using this to debug a certain opcode
    if ((opcode & 0xF000) == 0xF000 && (opcode & 0x00FF) == 0x0055) {
        //debugger_stepping = true;
    }

    draw_debugger_win("Before Handler");

    switch (opcode & 0xF000) {
        case 0x0000:
            switch (opcode & 0x00FF) {
                case 0x00E0:
                    //00E0: Clear the screen
                    memset(gfx, 0, sizeof(gfx));
                    draw_game = true;
                    pc += sizeof(opcode);
                    break;
                case 0x00EE:
                    //00EE: Return from a subroutine
                    pc = stack[--sp] + sizeof(opcode);
                    break;
                default:
                    if (opcode == 0x0000) {
                        //0NNN: Ignore this since it's ignored by most interpreters now
                        break;
                    }

                    log_write("Unhandled 0x0000 opcode 0x%04X", opcode);
                    return false;
            }

            break;
        case 0x1000:
            //1NNN: Jump to address NNN
            pc = opcode & 0x0FFF;
            break;
        case 0x2000:
            //2NNN: Execute subroutine starting at address NNN
            stack[sp++] = pc;
            pc = opcode & 0x0FFF;
            break;
        case 0x3000:
            //3XNN: Skip the following instruction if the value of register VX equals NN
            pc += sizeof(opcode);
            if (V[(opcode & 0x0F00) >> 8] == (opcode & 0x00FF)) {
                pc += sizeof(opcode);
            }
            break;
        case 0x4000:
            //4XNN: Skip the following instruction if the value of register VX is not equal to NN
            pc += sizeof(opcode);
            if (V[(opcode & 0x0F00) >> 8] != (opcode & 0x00FF)) {
                pc += sizeof(opcode);
            }
            break;
        case 0x5000:
            //Skip the following instruction if the value of register VX is equal to the value of register VY
            pc += sizeof(opcode);
            if (V[(opcode & 0x0F00) >> 8] == V[(opcode & 0x00F0) >> 4]) {
                pc += sizeof(opcode);
            }
            break;
        case 0x6000:
            //6XNN: Sets V[X] to NN
            V[(opcode & 0x0F00) >> 8] = opcode & 0x00FF;
            pc += sizeof(opcode);
            break;
        case 0x7000:
            //7XNN: Adds NN to V[X]
            V[(opcode & 0x0F00) >> 8] += opcode & 0x00FF;
            pc += sizeof(opcode);
            break;
        case 0x8000:
            x = (opcode & 0x0F00) >> 8;
            y = (opcode & 0x00F0) >> 4;

            switch (opcode & 0x000F) {
                case 0x0000:
                    //8XY0 - Sets VX to the value of VY.
                    V[x] = V[y];
                    pc += sizeof(opcode);
                    break;
                case 0x0001:
                    //8XY1 - Sets VX to (VX OR VY).
                    V[x] |= V[y];
                    pc += sizeof(opcode);
                    break;
                case 0x0002:
                    //8XY2 - Sets VX to (VX AND VY).
                    V[x] &= V[y];
                    pc += sizeof(opcode);
                    break;
                case 0x0003:
                    // 8XY3 - Sets VX to (VX XOR VY).
                    V[x] ^= V[y];
                    pc += sizeof(opcode);
                    break;
                case 0x0004:
                    //8XY4 - Adds VY to VX. VF is set to 1 when there's a carry, and to 0 when there isn't.
                    V[x] += V[y];
                    if(V[y] > (0xFF - V[x])) {
                        V[0xF] = 1;
                    }
                    else {
                        V[0xF] = 0;
                    }

                    pc += sizeof(opcode);
                    break;
                case 0x0005:
                    // 8XY5 - VY is subtracted from VX. VF is set to 0 when there's a borrow, and 1 when there isn't.
                    if(V[y] > V[x]) {
                        V[0xF] = 0;
                    }
                    else {
                        V[0xF] = 1;
                    }
                    V[x] -= V[y];
                    pc += sizeof(opcode);
                    break;
                case 0x0006:
                    // 0x8XY6 - Shifts VX right by one. VF is set to the value of the least significant bit of VX before the shift.
                    V[0xF] = V[x] & 0x1;
                    V[x] >>= 1;
                    pc += sizeof(opcode);
                    break;
                case 0x0007:
                    // 0x8XY7: Sets VX to VY minus VX. VF is set to 0 when there's a borrow, and 1 when there isn't.
                    if(V[x] > V[y]) {
                        V[0xF] = 0;
                    }
                    else {
                        V[0xF] = 1;
                    }

                    V[x] = V[y] - V[x];
                    pc += sizeof(opcode);
                    break;
                case 0x000E:
                    // 0x8XYE: Shifts VX left by one. VF is set to the value of
                    // the most significant bit of VX before the shift.
                    V[0xF] = V[x] >> 7;
                    V[(opcode & 0x0F00) >> 8] <<= 1;
                    pc += sizeof(opcode);
                    break;
                default:
                    log_write("Unhandled 0x8000 opcode 0x04%X", opcode);
                    return false;
            }
            break;
        case 0x9000:
            //9XY0: Skip the following instruction if the value of register VX is not equal to the value of register VY
            pc += sizeof(opcode);
            if (V[(opcode & 0x0F00) >> 8] != V[(opcode & 0x00F0) >> 4]) {
                pc += sizeof(opcode);
            }
            break;
        case 0xA000:
            //ANNN: Sets I to the address NNN
            I = opcode & 0x0FFF;
            pc += sizeof(opcode);
            break;
        case 0xB000:
            //BNNN: Jumps to NNN + V0
            pc = (opcode & 0x0FFF) + V[0];
            break;
        case 0xC000:
            //CXNN: Sets VX to a random number masked by NN.
            V[(opcode & 0x0F00) >> 8] = (rand() % (0xFF + 1)) & (opcode & 0x0FF);
            pc += sizeof(opcode);
            break;
        case 0xD000:
            //DYXN: Draws a sprite at coordinate (V[X],V[Y]) that has a width of 8 pixels and a height of N pixels
            x = V[(opcode & 0x0F00) >> 8];
            y = V[(opcode & 0x00F0) >> 4];
            height = opcode & 0x000F;

            V[0xF] = 0;
            for (yy = 0; yy < height; yy++) {
                pixel = memory[I + yy];
                for (xx = 0; xx < 8; xx++) {
                    if ((pixel & (0x80 >> xx)) != 0) {
                        if (gfx[x + xx + ((y + yy) * 64)] == 1) {
                            V[0xF] = 1;
                        }

                        gfx[x + xx + ((y + yy) * 64)] ^= 1;
                    }
                }
            }

            draw_game = true;
            pc += sizeof(opcode);

            break;
        case 0xE000:
            //EX..
            x = (opcode & 0x0F00) >> 8;

            switch (opcode & 0x00FF) {
                case 0x009E:
                    //EX9E: Skips the next instruction if the key stored in VX is pressed
                    pc += sizeof(opcode);
                    if (key[V[x]] != 0) {
                        pc += sizeof(opcode);
                    }
                    break;
                case 0x00A1:
                    //EXA1: Skips the next instruction if the key stored in VX is not pressed
                    pc += sizeof(opcode);
                    if (key[V[x]] == 0) {
                        pc += sizeof(opcode);
                    }
                    break;
                default:
                    log_write("Unhandled 0xE000 opcode 0x%04X", opcode);
                    return false;
            }

            break;
        case 0xF000:
            //FX..
            x = (opcode & 0x0F00) >> 8;

            switch (opcode & 0x00FF) {
                case 0x0007:
                    //FX07: Sets V[X] to the value of the delay timer
                    pthread_rwlock_rdlock(&dt_lock);
                    V[x] = dt;
                    pthread_rwlock_unlock(&dt_lock);

                    pc += sizeof(opcode);
                    break;
                case 0x000A:
                    //FX0A: Key press awaited, stored in V[X]
                    press = false;
                    for (i = 0; i < 16 && !press; i++) {
                        if (key[i] != 0) {
                            V[x] = i;
                            press = true;
                        }
                    }

                    if (press) {
                        pc += sizeof(opcode);
                    }

                    break;
                case 0x0015:
                    //FX15: Sets the delay timer to V[X]
                    pthread_rwlock_wrlock(&dt_lock);
                    dt = V[x];
                    pthread_rwlock_unlock(&dt_lock);

                    pc += sizeof(opcode);
                    break;
                case 0x0018:
                    //FX18: Sets the sound timer to V[X]
                    pthread_rwlock_wrlock(&st_lock);
                    st = V[x];
                    pthread_rwlock_unlock(&st_lock);

                    pc += sizeof(opcode);
                    break;
                case 0x001E:
                    //FX1E: V[F] is set to 1 when there's an overflow, otherwise 0
                    if (I + V[x] > 0xFFF) {
                        V[0xF] = 1;
                    }
                    else {
                        V[0xF] = 0;
                    }

                    I += V[x];
                    pc += sizeof(opcode);
                    break;
                case 0x0029:
                    //FX29: Sets I to the location of the sprite for the character in V[X]. Characters 0-F are represented by a 4x5 font
                    I = V[x] * 0x5;
                    pc += sizeof(opcode);
                    break;
                case 0x0033:
                    //FX33: Stores the binary encoded decimal representation of V[X] at the addresses I, I+1, I+2
                    memory[I] = V[x] / 100;
                    memory[I + 1] = (V[x] / 10) % 10;
                    memory[I + 2] = V[x] % 10;
                    pc += sizeof(opcode);
                    break;
                case 0x0055:
                    //FX55: Stores V[0] - V[X] in memory starting at address I
                    for (i = 0; i <= x; i++) {
                        memory[I + i] = V[i];
                    }

                    I += x + 1;
                    pc += sizeof(opcode);
                    break;
                case 0x0065:
                    //FX65: 
                    for (i = 0; i <= x; i++) {
                        V[i] = memory[I + i];
                    }

                    I += x + 1;
                    pc += sizeof(opcode);
                    break;
                default:
                    log_write("Unhandled 0xF000 opcode 0x%04X", opcode);
                    return false;
            }

            break;
        default:
            log_write("Unhandled opcode 0x04%X", opcode);
            return false;
    }

    draw_debugger_win("After Handler");
    return true;
}

//these timers always count at 60Hz
static void *
handle_timers(void *ptr) {
    uint64_t frame_start, diff;
    double ms_per_frame;
    bool do_beep;

    ms_per_frame = 1000.0 / 60.0;

    while (looping) {
        frame_start = time_ms();

        pthread_rwlock_wrlock(&dt_lock);
        if (dt > 0) {
            --dt;
        }
        pthread_rwlock_unlock(&dt_lock);

        do_beep = false;
        pthread_rwlock_wrlock(&st_lock);
        if (st > 0) {
            if (st == 1) {
                do_beep = true;
            }
            --st;
        }
        pthread_rwlock_unlock(&st_lock);
        if (do_beep) {
            beep();
        }

        diff = time_ms() - frame_start;
        if (diff < ms_per_frame) {
            usleep(1000 * (ms_per_frame - diff));
        }
    }

    return NULL;
}


//ncurses doesn't have good keyboard support so our keyboard handling is going to 
//be a little slow to respond
//simulate keyup and keydown
//keyup occurs after 100ms of it not being down
static void *
handle_keyboard(void *ptr) {
    uint64_t timers[16], now;
    int i, c;

    memset(timers, 0, sizeof(timers));

    while (looping) {
        while ((c = wgetch(stdscr)) != ERR) {
            for (i = 0; i < 16; i++) {
                if (c == key_map[i]) {
                    key[i] = 1;
                    timers[i] = time_ms() + 100;
                    break;
                }
            }
        }

        now = time_ms();
        for (i = 0; i < 16; i++) {
            if (timers[i] > 0 && now >= timers[i]) {
                key[i] = 0;
                timers[i] = 0;
            }
        }

        usleep(1000 * 1);
    }

    return NULL;
}

static void
usage(const char *fmt, ...) {
    va_list ap;

    if (fmt != NULL) {
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);

        fputc('\n', stdout);
    }

    puts("Usage: chip8 [options] <rom path>");
    puts("Options:");
    puts(" -f <fps>    Set the frames per second of the CPU. Certain games run better with");
    puts("             higher values. The default is 120.");
    puts(" -c <color>  Sets the color of the pixels. The default is green.");
    puts("             Valid colors: red, green, blue, yellow, magenta, cyan, white.");
}

static bool
parse_args(int argc, char **argv) {
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            opt_fps = atoi(argv[++i]);
            if (opt_fps < 60) {
                usage("FPS cannot be lower than 60");
                return false;
            }
        }
        else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            ++i;
            if (strcmp(argv[i], "red") == 0) {
                opt_color = COLOR_RED;
            }
            else if (strcmp(argv[i], "green") == 0) {
                opt_color = COLOR_GREEN;
            }
            else if (strcmp(argv[i], "blue") == 0) {
                opt_color = COLOR_BLUE;
            }
            else if (strcmp(argv[i], "yellow") == 0) {
                opt_color = COLOR_YELLOW;
            }
            else if (strcmp(argv[i], "magenta") == 0) {
                opt_color = COLOR_MAGENTA;
            }
            else if (strcmp(argv[i], "cyan") == 0) {
                opt_color = COLOR_CYAN;
            }
            else if (strcmp(argv[i], "white") == 0) {
                opt_color = COLOR_WHITE;
            }
            else {
                usage("Invalid color value");
                return false;
            }
        }
        else {
            opt_path = argv[i];
            break;
        }
    }

    if (opt_path == NULL) {
        usage("No ROM path given");
        return false;
    }

    return true;
}

int
main(int argc, char **argv) {
    uint64_t frame_start, diff;
    bool success = true;
    double ms_per_frame;

    if (!parse_args(argc, argv)) {
        return 1;
    }

    srand(time(NULL));

    initscr();
    noecho();
    curs_set(0);

    start_color();
    init_pair(1, opt_color, opt_color);

    initialize();
    success = load();

    if (success) {
        pthread_create(&thread_timers, NULL, handle_timers, NULL);
        pthread_create(&thread_keys, NULL, handle_keyboard, NULL);
    }

    ms_per_frame = 1000.0 / (double)opt_fps;
    program_start = time(NULL);

    while (success && looping) {
        frame_start = time_ms();
        success = cycle();

        if (draw_game) {
            draw_game_win();
            draw_game = false;
        }

        if (!success) {
            log_write("Press any key to quit");
        }

        if (draw_log) {
            draw_log_win();
            draw_log = false;
        }

        if (!success) {
            break;
        }

        diff = time_ms() - frame_start;
        if (diff < ms_per_frame) {
            usleep(1000 * (ms_per_frame - diff));
        }

        ++counter_frames;
    }

    looping = false;

    if (!success) {
        draw_log_win();
        fgetc(stdin);
    }

    pthread_join(thread_timers, NULL);
    pthread_join(thread_keys, NULL);
    pthread_rwlock_destroy(&dt_lock);
    pthread_rwlock_destroy(&st_lock);
    delwin(win_game);
    delwin(win_log);
    delwin(win_debugger);
    endwin();

    return success ? 0 : 1;
}
