
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <raylib.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <regex.h>
#include <string.h>



bool SETTINGS_DISPLAY_CTRL_CHARACTERS = false;

#define WIDTH 80
#define HEIGHT 20

unsigned long long frame = 0;
unsigned char user_input_arr[100], user_input_arr_len=0;
unsigned char driver_output_arr[100], driver_output_arr_len=0;

Font font;

typedef struct {
    unsigned char ascii_code;
} Cell;

typedef struct {
    bool is_a_valid_sequence;
    int length_in_bytes;
    char last_letter;
    int parameters[2]; // we take only the first two
} EscapeSequence;

Cell screen[HEIGHT][WIDTH]; // should be initialized before usage!

int terminal_cursor_x = 0, terminal_cursor_y = 0; // this points to the next empty cell
int terminal_driver_fd;
bool need_to_redraw_screen = true;



void initialize_the_screen(void);
void initialize_the_screen_with_test_data(void);
void scroll_the_screen_upwards_by_one(void);
void redraw_screen(void);
void add_to_user_input_array( unsigned char ascii_code );
void print_user_input_array(void);
void pass_user_input_to_driver(void);
void update_terminal();
void handle_user_kbd_input(void);
void contact_terminal_driver(void);
void parse_escape_sequence( EscapeSequence *seq, unsigned char *str, unsigned int str_length);



void initialize_the_screen(void) {
    for (int y=0; y<HEIGHT; y++) {
        for (int x=0; x<WIDTH; x++) {
            Cell *c = &screen[y][x];
            c->ascii_code = ' ';
        }
    }
}

void initialize_the_screen_with_test_data(void) {
    initialize_the_screen();
    for (int i = 0; i<256; i++) {
        int derived_x = i%WIDTH;
        int derived_y = i/WIDTH;
        Cell *c = &screen[derived_y][derived_x];
        c->ascii_code = i;
    }
    for (int i = 256; i<512; i++) {
        int derived_x = i%WIDTH;
        int derived_y = i/WIDTH;
        Cell *c = &screen[derived_y][derived_x];
        c->ascii_code = i;
    }
}

void scroll_the_screen_upwards_by_one(void) {
    for (int y = 1; y<HEIGHT; y++) {
        for (int x = 0; x<WIDTH; x++) {
            screen[y-1][x].ascii_code = screen[y][x].ascii_code;
        }
    }
    for (int x = 0; x<WIDTH; x++) {
        screen[HEIGHT-1][x].ascii_code = ' ';
    }
}


void redraw_screen(void) {
    for (int y=0; y<HEIGHT; y++) {
        for (int x=0; x<WIDTH; x++) {
            Cell c = screen[y][x];
            bool is_inverted_color = terminal_cursor_x==x && terminal_cursor_y==y;
            bool is_a_control_character = c.ascii_code<0x20 || c.ascii_code>=0x7f&&c.ascii_code<=0xa0 || c.ascii_code==0xad;
            Vector2 top_left_corner_px = {x*10, y*25};

            // draw the background
            DrawRectangleV(
                top_left_corner_px,
                (Vector2){10,25},
                is_inverted_color ? WHITE : BLACK
            );

            // draw the glyph
            DrawTextCodepoint(
                font,
                c.ascii_code,
                top_left_corner_px,
                is_a_control_character ? 10 : 20,
                is_inverted_color ? BLACK : WHITE
            );
        }
    }
    need_to_redraw_screen = false;
}


void add_to_user_input_array( unsigned char ascii_code ) {
    if (user_input_arr_len==100) return;
    user_input_arr[ user_input_arr_len ] = ascii_code;
    user_input_arr_len++;
}

void print_user_input_array(void) {
    if (user_input_arr_len==0) return;
    printf("[");
    for (int i = 0; i<user_input_arr_len; i++) {
        printf("%hhx,", user_input_arr[i]);
    }
    puts("]");
}

void pass_user_input_to_driver(void) {
    // discard the bytes that we couldnt pass to the terminal driver
    write(terminal_driver_fd, user_input_arr, user_input_arr_len);
    // empty the array afterwards
    user_input_arr_len = 0;
}

void handle_driver_output(void) {
    // while there are still bytes unread...
    while (true) {
        // empty the array first
        driver_output_arr_len = 0;
        // try to read 100 bytes. note that we made the reads non-blocking
        int bytes_read = read(terminal_driver_fd, driver_output_arr, 100);
        // if no input, we're done
        if (bytes_read<1) break;
        driver_output_arr_len = bytes_read;
        update_terminal();
    }
}


void update_terminal() {
    
    need_to_redraw_screen = true;
    for (int i = 0; i<driver_output_arr_len; i++) {
        unsigned char c = driver_output_arr[i];

        if (c=='\n') {
            // move the cursor to its new position
            int new_x, new_y;
            if (terminal_cursor_y==HEIGHT-1) {
                scroll_the_screen_upwards_by_one();
                new_x = 0;
                new_y = HEIGHT-1;
            }
            else {
                new_x = 0;
                new_y = terminal_cursor_y+1;
            }

            terminal_cursor_x = new_x;
            terminal_cursor_y = new_y;

        }
        else if (c=='\r') {
            // move to its new spot
            int new_x = 0;
            int new_y = terminal_cursor_y;
            terminal_cursor_x = new_x;
            terminal_cursor_y = new_y;
        }
        else if (c=='\b') {
            // clear the character
            if (terminal_cursor_x==0) {
                screen[terminal_cursor_y][terminal_cursor_x].ascii_code = ' ';
            }
            else {
                screen[terminal_cursor_y][terminal_cursor_x-1].ascii_code = ' ';
            }

            // move the cursor to its new position
            int new_x, new_y;
            if (terminal_cursor_x==0) {
                new_x = 0;
                new_y = terminal_cursor_y;
            }
            else {
                new_x = terminal_cursor_x-1;
                new_y = terminal_cursor_y;
            }

            terminal_cursor_x = new_x;
            terminal_cursor_y = new_y;
        }
        else if (c=='\t') {
            int new_x = (terminal_cursor_x / 8 + 1) * 8;
            if (new_x >= WIDTH) new_x = WIDTH-1;
            terminal_cursor_x = new_x;
        }
        else if (c=='\x1b') {
            // escape sequence!
            EscapeSequence s;
            parse_escape_sequence(&s, &driver_output_arr[i], driver_output_arr_len-i);

            if (!s.is_a_valid_sequence) {
                goto place_character;
            }
            
            switch(s.last_letter) {
            case 'A': { // cursor n up
                terminal_cursor_y -= s.parameters[0] || 1;
                if (terminal_cursor_y<0) terminal_cursor_y=0;
                break;
            }
            case 'B': { // cursor n down
                terminal_cursor_y += s.parameters[0] || 1;
                if (terminal_cursor_y>HEIGHT-1) terminal_cursor_y=HEIGHT-1;
                break;
            }
            case 'C': { // cursor n right
                terminal_cursor_x += s.parameters[0] || 1;
                if (terminal_cursor_x>WIDTH-1) terminal_cursor_x=WIDTH-1;
                break;
            }
            case 'D': { // cursor n left
                terminal_cursor_x -= s.parameters[0] || 1;
                if (terminal_cursor_x<0) terminal_cursor_x=0;
                break;
            }
            case 'E': { // cursor n lines down
                terminal_cursor_x = 0;
                terminal_cursor_y += s.parameters[0] || 1;
                if (terminal_cursor_y>HEIGHT-1) terminal_cursor_y=HEIGHT-1;
                break;
            }
            case 'F': { // cursor n lines up
                terminal_cursor_x = 0;
                terminal_cursor_y -= s.parameters[0] || 1;
                if (terminal_cursor_y<0) terminal_cursor_y=0;
                break;
            }
            case 'G': { // cursor to column n
                terminal_cursor_x = s.parameters[0] || 1;
                if (terminal_cursor_x<0) terminal_cursor_x = 0;
                if (terminal_cursor_x>WIDTH-1) terminal_cursor_x = WIDTH-1;
                break;
            }
            case 'H': { // cursor to row n, column m
                printf("h %d %d,\n", s.parameters[0], s.parameters[1]);
                terminal_cursor_y = s.parameters[0] ? (s.parameters[0]-1) : 0;
                terminal_cursor_x = s.parameters[1] ? (s.parameters[1]-1) : 0;
                if (terminal_cursor_x<0) terminal_cursor_x = 0;
                if (terminal_cursor_x>WIDTH-1) terminal_cursor_x = WIDTH-1;
                if (terminal_cursor_y<0) terminal_cursor_y = 0;
                if (terminal_cursor_y>HEIGHT-1) terminal_cursor_y = HEIGHT-1;
                break;
            }
            case 'J': {
                if (s.parameters[0]==0) {
                    // clear from cursor to end of screen
                    for (int i = WIDTH*terminal_cursor_y+terminal_cursor_x; i<WIDTH*HEIGHT; i++) {
                        screen[i/WIDTH][i%WIDTH].ascii_code = ' ';
                    }
                }
                else if (s.parameters[0]==1) {
                    // clear from start of screen to cursor
                    for (int i = 0; i<=WIDTH*terminal_cursor_y+terminal_cursor_x; i++) {
                        screen[i/WIDTH][i%WIDTH].ascii_code = ' ';
                    }
                }
                else if (s.parameters[0]==2 || s.parameters[0]==3) {
                    // clear entire screen
                    for (int i = 0; i<WIDTH*HEIGHT; i++) {
                        screen[i/WIDTH][i%WIDTH].ascii_code = ' ';
                    }
                }
                break;
            }
            case 'K': {
                if (s.parameters[0]==0) {
                    // clear from cursor to end of line
                    for (int i = terminal_cursor_x; i<WIDTH; i++) {
                        screen[terminal_cursor_y][i].ascii_code = ' ';
                    }
                }
                else if (s.parameters[0]==1) {
                    // clear from cursor to end of line
                    for (int i = 0; i<=terminal_cursor_x; i++) {
                        screen[terminal_cursor_y][i].ascii_code = ' ';
                    }
                }
                else if (s.parameters[0]==2) {
                    // clear entire line
                    for (int i = 0; i<WIDTH; i++) {
                        screen[terminal_cursor_y][i].ascii_code = ' ';
                    }
                }
                break;
            }
            case 'S': {
                // scroll up
                for (int i = 0; i<s.parameters[0]; i++) {
                    scroll_the_screen_upwards_by_one();
                }
                break;
            }
            default: {}
            }
            
            // skip ahead. -1 accounts for the i++
            i += s.length_in_bytes-1;
        }
        else {

            place_character:

            if (!SETTINGS_DISPLAY_CTRL_CHARACTERS) {
                if (c>=0 && c<=31 || c==0x7f) {
                    continue;
                }
            }
            
            // place the character, remove the cursor from there
            screen[terminal_cursor_y][terminal_cursor_x].ascii_code = c;

            // move the cursor to its new position
            int new_x, new_y;
            if (terminal_cursor_x==WIDTH-1 && terminal_cursor_y==HEIGHT-1) {
                scroll_the_screen_upwards_by_one();
                new_x = 0;
                new_y = HEIGHT-1;
            }
            else if (terminal_cursor_x==WIDTH-1) {
                new_x = 0;
                new_y = terminal_cursor_y+1;
            }
            else {
                new_x = terminal_cursor_x+1;
                new_y = terminal_cursor_y;
            }

            terminal_cursor_x = new_x;
            terminal_cursor_y = new_y;
        }

    }
}

void handle_user_kbd_input(void) {

    // empty the array first
    user_input_arr_len = 0;

    // register all characters, changing from unicode codepoints to utf8
    while( true ) {
        int character = GetCharPressed();
        if (!character) break;

        int returned_length;
        const char *returned_bytes = CodepointToUTF8(character, &returned_length);

        for (int i = 0; i<returned_length; i++) {
            add_to_user_input_array(returned_bytes[i] );
        }
    }

    // register special keys that don't have a visual representation
    while( true ) {
        int key = GetKeyPressed();
        if (!key) break;

        bool ctrl_is_pressed = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
        if (key==KEY_TAB) {
            add_to_user_input_array('\t');
        }
        else if (key==KEY_BACKSPACE) {
            add_to_user_input_array('\b'); // or \7f?
        }
        else if (key==KEY_ENTER || key==KEY_KP_ENTER) {
            add_to_user_input_array('\n'); // or \r?
        }        
        else if (key>=KEY_A && key<=KEY_Z && ctrl_is_pressed) {
            add_to_user_input_array(key - 64);
        }
        // arrow keys
        else if (key==KEY_UP) {
            add_to_user_input_array('\x1b'); // or maybe do VT52 mode instead of ANSI?
            add_to_user_input_array('[');
            add_to_user_input_array('A');
        }
        else if (key==KEY_DOWN) {
            add_to_user_input_array('\x1b');
            add_to_user_input_array('[');
            add_to_user_input_array('B');
        }
        else if (key==KEY_RIGHT) {
            add_to_user_input_array('\x1b');
            add_to_user_input_array('[');
            add_to_user_input_array('C');
        }
        else if (key==KEY_LEFT) {
            add_to_user_input_array('\x1b');
            add_to_user_input_array('[');
            add_to_user_input_array('D');
        }
        
    }
}


void contact_terminal_driver(void) {

    struct winsize terminal_sizes = {
        .ws_row=HEIGHT, .ws_col=WIDTH, .ws_xpixel=10, .ws_ypixel=25
    };

    // normally we should do the settings manually, but for now we just copy the controlling terminal
    struct termios terminal_initial_settings;
    tcgetattr(STDOUT_FILENO, &terminal_initial_settings);

    int result = forkpty(&terminal_driver_fd, NULL, &terminal_initial_settings, &terminal_sizes);
    if (result == -1) {
        puts("forkpty failed.");
        exit(1);
    }
    else if (result == 0) { // child process (shell)
        char* arguments[1] = {NULL};
        char* environment_variables[2] = {"TERM=linux", NULL};
        
        execve("/bin/bash", arguments, environment_variables);
        // this line is reached only if the bash command fails
        puts("bash failed.");
        exit(1);
    }
    
    // only the parent process (terminal) can reach here

    // change terminal_driver_fd to non-blocking read mode
    int flags = fcntl(terminal_driver_fd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl(F_GETFL) failed");
        exit(1);
    }

    // Add the O_NONBLOCK flag to enable non-blocking mode
    result = fcntl(terminal_driver_fd, F_SETFL, flags | O_NONBLOCK);
    if ( result == -1) {
        perror("fcntl(F_SETFL) failed");
        exit(1);
    }

    
}


int main(void) {

    initialize_the_screen();
    InitWindow(800, 500, "My Terminal");

    // tell raylib to load characters whose ascii code is 0..255
    int codepoints_to_load[256];
    for (int i = 0; i<256; i++) {
        codepoints_to_load[i] = i;
    }

    font = LoadFontEx("font/GNU_Unifont_Modified.otf", 20, codepoints_to_load, 256);
    if (font.glyphCount==0) {
        font = GetFontDefault();
    }
    SetExitKey(KEY_NULL);

    int x = 100, y = 100;

    SetTargetFPS(60);

    contact_terminal_driver();

    while (!WindowShouldClose()) {
        BeginDrawing();

            handle_user_kbd_input();
            pass_user_input_to_driver();
            handle_driver_output();
            if(need_to_redraw_screen) {
                redraw_screen();
            }

        EndDrawing();
        frame++;
    }

    CloseWindow();
    return EXIT_SUCCESS;
}


void parse_escape_sequence( EscapeSequence *seq, unsigned char *str, unsigned int str_length) {

    if (str_length==0 || str_length==1) {
        seq->is_a_valid_sequence = false;
        return;
    }

    if (str[1]==']') {
        // presumably an OSC type of escape. starts with ] and ends with Ctrl+G (0x07)
        // ignore these
        
        int index_of_ctrlG = 2;
        for (int i = 2; i<str_length; i++, index_of_ctrlG++) {
            if (str[i]=='\x07') {
                break;
            }
        }

        seq->is_a_valid_sequence = true;
        seq->length_in_bytes = index_of_ctrlG+1;
        seq->last_letter = '\x07';
        return;
    }
    else if (str[1] == '[') {
        // probably CSI type

        char *regexPattern = "^\x1b\\[\\??([0-9]*)(;([0-9]*))?([a-zA-Z])";
            
        const size_t maxGroups = 5;

        regex_t regexCompiled;
        regmatch_t groupArray[maxGroups];

        if (regcomp(&regexCompiled, regexPattern, REG_EXTENDED)) {
            printf("Could not compile regular expression.\n");
            seq->is_a_valid_sequence = false;
            return;
        };

        if (regexec(&regexCompiled, (char*)str, maxGroups, groupArray,
                    0)) { // No matches
            seq->is_a_valid_sequence = false;
            return;
        }

        seq->is_a_valid_sequence = true;
        seq->last_letter = str[ groupArray[4].rm_so ];

        if (groupArray[1].rm_eo != groupArray[1].rm_so) {
            sscanf((char*)str+groupArray[1].rm_so, "%u", &seq->parameters[0]);
        }
        else {
            seq->parameters[0] = 0;
        }
        if (groupArray[3].rm_eo != groupArray[3].rm_so) {
            sscanf((char*)str+groupArray[3].rm_so, "%u", &seq->parameters[1]);
        }
        else {
            seq ->parameters[1] = 0;
        }

        seq->length_in_bytes = groupArray[0].rm_eo - groupArray[0].rm_so;

        regfree(&regexCompiled);
        return;
    }
    else {
        // neither OSC, not CSI
        seq->is_a_valid_sequence = false;
        return;
    }

}