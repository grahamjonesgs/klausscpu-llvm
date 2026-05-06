/* adventure.c — Mini text adventure for KlaussCPU (LLVM backend port).
 *
 * Compile via Makefile:  make adventure.elf
 *
 * Commands: n/s/e/w = move, l = look, g = get, u = use, i = inventory, q = quit
 * Goal: find torch, find key (needs torch), unlock door, escape!
 */

#include <stdio.h>

/* ── Game state ──────────────────────────────────────────────────────────── */

int room;
int items;
int turns;
int alive;

#define ROOM_CELL    0
#define ROOM_HALL    1
#define ROOM_ARMORY  2
#define ROOM_DOOR    3
#define ROOM_FREE    4

#define HAS_TORCH    1
#define HAS_KEY      2

/* Read one command character from UART */
int read_cmd(void)
{
    int c;
    int cmd;

    printf("> ");
    fflush(stdout);
    cmd = getchar();
    putchar(cmd);

    /* Drain rest of line until CR or LF */
    if (cmd != 13 && cmd != 10) {
        c = cmd;
        while (c != 13 && c != 10)
            c = getchar();
    }
    putchar('\n');

    /* Uppercase to lowercase */
    if (cmd >= 'A' && cmd <= 'Z')
        cmd = cmd + 32;

    return cmd;
}

/* Room descriptions */
void look(void)
{
    if (room == ROOM_CELL) {
        puts("You are in a cold stone cell.");
        puts("Moonlight seeps through a crack.");
        if (!(items & HAS_TORCH))
            puts("A rusty TORCH sits on the wall.");
        puts("A passage leads NORTH.");
    }
    else if (room == ROOM_HALL) {
        puts("You stand in a long hallway.");
        if (items & HAS_TORCH) {
            puts("Your torch reveals wall scratches:");
            puts("THE ARMORY HIDES SECRETS");
        }
        else {
            puts("It is very dark here.");
        }
        puts("Exits: S=cell E=armory N=door");
    }
    else if (room == ROOM_ARMORY) {
        puts("You enter a dusty armory.");
        puts("Broken swords line the walls.");
        if (!(items & HAS_KEY)) {
            if (items & HAS_TORCH)
                puts("Torchlight glints off a brass KEY!");
            else
                puts("Something metallic glints...");
        }
        else {
            puts("Nothing else of interest here.");
        }
        puts("Exit: W=hallway");
    }
    else if (room == ROOM_DOOR) {
        puts("A massive iron door blocks the way.");
        puts("There is a keyhole.");
        if (items & HAS_KEY) {
            puts("The brass key might fit...");
            puts("Type u to USE the key.");
        }
        else {
            puts("It is locked. You need a key.");
        }
        puts("Exit: S=hallway");
    }
}

/* Inventory */
void show_inventory(void)
{
    printf("You carry: ");
    if (items == 0)          printf("nothing");
    if (items & HAS_TORCH)   printf("[torch] ");
    if (items & HAS_KEY)     printf("[key] ");
    putchar('\n');
}

/* Pick up items */
void do_get(void)
{
    if (room == ROOM_CELL && !(items & HAS_TORCH)) {
        items = items | HAS_TORCH;
        puts("You take the torch. Flames dance!");
    }
    else if (room == ROOM_ARMORY && !(items & HAS_KEY)) {
        if (items & HAS_TORCH) {
            items = items | HAS_KEY;
            puts("You pick up the brass key.");
        }
        else {
            puts("You fumble in the dark...");
        }
    }
    else {
        puts("Nothing to pick up here.");
    }
}

/* Use items */
void do_use(void)
{
    if (room == ROOM_DOOR && (items & HAS_KEY)) {
        puts("You insert the key... CLICK!");
        puts("The door groans open. Starlight!");
        room = ROOM_FREE;
    }
    else {
        puts("Nothing useful to do here.");
    }
}

/* Movement */
void do_move(int dir)
{
    if (room == ROOM_CELL) {
        if (dir == 'n') { room = ROOM_HALL; }
        else { puts("You cannot go that way."); return; }
    }
    else if (room == ROOM_HALL) {
        if      (dir == 's') { room = ROOM_CELL;   }
        else if (dir == 'e') { room = ROOM_ARMORY; }
        else if (dir == 'n') { room = ROOM_DOOR;   }
        else { puts("You cannot go that way."); return; }
    }
    else if (room == ROOM_ARMORY) {
        if (dir == 'w') { room = ROOM_HALL; }
        else { puts("You cannot go that way."); return; }
    }
    else if (room == ROOM_DOOR) {
        if (dir == 's') { room = ROOM_HALL; }
        else { puts("You cannot go that way."); return; }
    }
    look();
}

/* Main */
int main(void)
{
    int cmd;

    room  = ROOM_CELL;
    items = 0;
    turns = 0;
    alive = 1;

    puts("========================================");
    puts("   DUNGEON ESCAPE");
    puts("   Running on KlaussCPU!");
    puts("========================================");
    puts("Commands: n/s/e/w=move l=look");
    puts("  g=get u=use i=inventory q=quit");
    putchar('\n');
    puts("You awaken on cold stone...");
    putchar('\n');
    look();

    while (alive) {
        cmd = read_cmd();
        turns = turns + 1;

        if      (cmd == 'q') { puts("You surrender to the darkness..."); alive = 0; }
        else if (cmd == 'l') { look(); }
        else if (cmd == 'i') { show_inventory(); }
        else if (cmd == 'g') { do_get(); }
        else if (cmd == 'u') { do_use(); }
        else if (cmd == 'n' || cmd == 's' || cmd == 'e' || cmd == 'w') { do_move(cmd); }
        else                 { puts("Huh? Try n/s/e/w/l/g/u/i/q"); }

        if (room == ROOM_FREE) {
            puts("\n*** YOU ESCAPED THE DUNGEON! ***");
            printf("Turns: %d\n", turns);
            alive = 0;
        }
    }

    puts("Game over.");
    return 0;
}
