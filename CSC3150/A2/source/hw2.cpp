#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <curses.h>
#include <termios.h>
#include <fcntl.h>

const int ROW = 11; 						    	/* height of screen */
const int COL = 50; 						    	/* width of screen */
const int RIVER_SIDE_LEN = 15;						/* hidden side length of river */
const int RIVER_LEN = COL + 2 * RIVER_SIDE_LEN; 	/* length of river */
const int RIVER_AREA = RIVER_LEN * (ROW - 2);   	/* area of river */

int 	  LOG_MAX_LEN = 8;				        	/* maximum log length, should be <= RIVER_SIDE_LEN */
int       LOG_LEN_VAR = 3;							/* random variation of log length from max len*/
float	  LOG_SLEEP_VAR = 0.5;						/* random variation of time interval of log drift from LOG_SLEEP_US */ 
int       LOG_MAX_INT = 5;							/* maximum of random time interval of log drift */
int 	  LOG_SLEEP_US = 1e5; 				    	/* time interval of log drift in us */
char 	  map[ROW][RIVER_LEN]; 			        	/* map of river */
char 	  status; 						  	    	/* game status = (w)in, (d)ie, (q)uit */
int       score = 0;                            	/* current score */
int 	  hi = 0; 							    	/* highest score */
int       speedrate = 50;							/* speed rate of log drift */

pthread_t pth_kb, pth_dlog[ROW-2], pth_slog[ROW-2]; /* threads fetching kb and updating logs */
pthread_mutex_t mutex;								/* mutex for log spawing threads */


// Utilities

/** Determine a keyboard is hit or not. 
  * If yes, return 1. Otherwise return 0. 
  */
int kbhit(void){
	struct termios oldt, newt;
	int ch;
	int oldf;

	tcgetattr(STDIN_FILENO, &oldt);

	newt = oldt;
	newt.c_lflag &= ~(ICANON | ECHO);

	tcsetattr(STDIN_FILENO, TCSANOW, &newt);
	oldf = fcntl(STDIN_FILENO, F_GETFL, 0);

	fcntl(STDIN_FILENO, F_SETFL, oldf | O_NONBLOCK);

	ch = getchar();

	tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
	fcntl(STDIN_FILENO, F_SETFL, oldf);

	if(ch != EOF) {
		ungetc(ch, stdin);
		return 1;
	}
	return 0;
}

/** Convert screen coord. (x, y) into map 
  * index and return a reference.
  */
char & at_map(int x, int y) {
	return map[y][x + LOG_MAX_LEN];
}

/* Return a rand int from [l, u]. */
int rand_in(int l, int u) {
	return rand() % (u - l + 1) + l;
}

/* Clear screen. Move cursor to topleft. */
void scflush(const char* str = NULL) {
	printf("\033[2J\033[1;1H");
	fflush(stdout);
	if (str) puts(str);
}

/** Flush stdin.
  * © https://stackoverflow.com/questions/17318886/fflush-is-not-working-in-linux/23885008#23885008
  */
void clean_stdin() {
	int stdin_cpy = dup(STDIN_FILENO);
	tcdrain(stdin_cpy);
	tcflush(stdin_cpy, TCIFLUSH);
	close(stdin_cpy);
}

/* Display n-sec count down */
void countdown(int n) {
	for (int i = n; i >= 1; i--) {
		printf("\33[%d;%dH%d\n", ROW/2+1, COL/2+1, i);
		usleep(1e6);
	}   printf("\33[%d;%dH%s\n", ROW/2+1, COL/2, "GO!");
		usleep(1e6);
	/* clear input during count down */
	clean_stdin();
}


// Frog object

class Frog{
public:
	/* coordinates on screen */
	int x, y;

	/* constructor */
	Frog(int _x = 0, int _y = 0) {
		x = _x; y = _y;
	}

	/* setter of frog's position. Return true if on log or bank */
	void jump_to(int _x, int _y) {
		x = _x; y = _y;
	}

	bool alive() {
		return (at_map(x, y) == '=' || at_map(x, y) == '|') \
				&& x >= 0 && x < COL;
	}

	void draw() {printf("\33[s\u001b[32m\33[%d;%dH%s\u001b[0m\33[u", y+1, x+1, "@");}
}; 
Frog frog = Frog();


// Threads

/* spawn log on row y of random length with xoff offset w.r.t the boundary */
void spawn_log_on_row(int y, int xoff = 0) {
	bool drift_right = (y % 2 == 0);
	int log_len = LOG_MAX_LEN + rand_in(-LOG_LEN_VAR, 0);
	if (drift_right)
		memset(&at_map(-log_len + xoff, y), '=', log_len);
	else
		memset(&at_map(COL - xoff, y), '=', log_len);
}

/** Thread function spawn_logs_on_row: For every random time interval, 
  * spawn a log at row *y 
  */
void* spawn_logs_on_row(void* y) {
	while (true) {
		spawn_log_on_row(*(int*)y);
		usleep(LOG_SLEEP_US * (1 - (speedrate - 50.0) / 100) * \
		      (LOG_MAX_LEN + 5 + rand_in(0, LOG_MAX_INT)));
	}
	// pthread_exit(NULL);    shall be exited by pthread_cancel()
}

/** Thread function drift_logs_on_row: For every random time interval, drift logs
  * on row *py, move & redraw frog if it's on that row.
  */
void* drift_logs_on_row(void* py) {

	int y = * (int*) py;
	int t_off = LOG_SLEEP_VAR * LOG_SLEEP_US;
	t_off = rand_in(-t_off, t_off);

	while (true) {
		usleep(LOG_SLEEP_US * (1 - (speedrate - 50.0) / 100) + t_off);

		/* even logs drift right */
		if (y % 2 == 0) {
			memmove(&map[y][1], &map[y][0], RIVER_LEN - 1);
			map[y][0] = ' ';
		} else {
		/* odd logs drift left */
			memmove(&map[y][0], &map[y][1], RIVER_LEN - 1);
			map[y][RIVER_LEN - 1] = ' ';
		}

		/* draw row. Need a mutex to avoid printf conflict */
		pthread_mutex_lock(&mutex);
		printf("\033[%d;1H", y + 1);
		for (int x = 0; x < COL; x++) {
			printf("%c", at_map(x, y));
		}
		pthread_mutex_unlock(&mutex);

		/* frog on this row of log. Move/redraw frog. */
		if (frog.y == y) {
			frog.x += (y % 2 == 0)? 1 : -1;
			frog.draw();
		}
	}
	// pthread_exit(NULL);    shall be exited by pthread_cancel()
}

/** Thread function check_kb: check kb indefinitely, if kb caught, update frog position,
  * redraw map, and check frog status, return if game ends.
  */
void* check_kb(void* _) {
	char dir;
	while (true) {
		if (kbhit()) {
			printf(
				"\33[%d;%dH%s", frog.y+1, frog.x+1, 
				(frog.y == ROW - 1)? "|" : "=");
			dir = getchar();
			switch (dir) {
				case 'w': case 'W':
					frog.jump_to(
						frog.x, frog.y-1); break;
				case 's': case 'S':
					frog.jump_to(
						frog.x, frog.y >= ROW - 1? \
						frog.y : frog.y + 1); break;
				case 'a': case 'A':
					frog.jump_to(
						frog.x <= 0? frog.x : frog.x - 1,
						frog.y); break;
				case 'd': case 'D':
					frog.jump_to(
						frog.x >= COL - 1? \
						frog.x : frog.x + 1, frog.y); break;
				case ',':
					if (speedrate == 99) speedrate -= 9;
					else if (speedrate > 10) speedrate -= 10;
					break;
				case '.':
					if (speedrate < 90) speedrate += 10;
					else if (speedrate == 90) speedrate += 9;
					break;
				case 'q': case 'Q':
					status = 'q';
					pthread_exit(NULL);
				default: break;
			}
			/* frog could have moved. redraw. */
			frog.draw();
		}
		
		/* update frog status, potentially exit*/
		if (!frog.alive()) {
			status = 'd';
			pthread_exit(NULL);
		}

		if (frog.y == 0) {
			status = 'w';
			pthread_exit(NULL);
		}

		/* update scores */
		score = ROW - 1 - frog.y;
		if (score > hi) hi = score;

		/* print scores and speed control */
		printf("\33[s\33[%d;%dH%d\33[u", 	 1, COL+1+5, hi);
		printf("\33[s\33[%d;%dH%d\33[u", 	 3, COL+1+6, score);
		printf("\33[s\33[%d;%dH%s%d%s\33[u", 6, COL+1  ,
			   "\33[33m[<]\33[0m  ", speedrate, "%  \33[33m[>]\33[0m");
	}
}


// Game Configuration

/* Ask the user to choose initial game difficulty. */
void choose_difficulty() {
	scflush("Choose difficulty...");
	puts("\33[33m[1]\33[0m EZ");
	puts("\33[33m[2]\33[0m Meh");
	puts("\33[33m[3]\33[0m CRAAAZY!");

	while (true) {
		if (kbhit()) {
			char difficulty = getchar();
			switch (difficulty) {
				case '1': 
				LOG_LEN_VAR = 3;
				LOG_MAX_LEN = 12;
				LOG_MAX_INT = 4;
				LOG_SLEEP_VAR = 0.5;
				LOG_SLEEP_US = 3 * 1e5; return;
				case '2': 
				LOG_LEN_VAR = 4;
				LOG_MAX_LEN = 10;
				LOG_MAX_INT = 5;
				LOG_SLEEP_VAR = 0.6;
				LOG_SLEEP_US = 8 * 1e4; return;
				case '3': 
				LOG_LEN_VAR = 4;
				LOG_MAX_LEN = 8;
				LOG_MAX_INT = 5;
				LOG_SLEEP_VAR = 0.6;
				LOG_SLEEP_US = 4 * 1e4; return;
				default: break;
			}
		}
	}
}

/* Initialize the map: fill river bank as '|' and river as ' '. */
void init_map() {
	memset(map, 0, sizeof(map)) ;
	memset(&map, '|', RIVER_LEN);
	memset(&map[ROW-1], '|', RIVER_LEN);
	memset(&map[1], ' ', RIVER_AREA);
}

/* Draw the starting screen */
void draw_init_screen() {
	scflush();

	/* draw bank/log/river */
	for(int y = 0; y < ROW; y++) {
		for (int x = 0; x < COL; x++)
			printf("%c", at_map(x, y));
		printf("\n");
	}
	frog.draw();

	/* print hi-score and current score */
	printf("\33[%d;%dH%s%d", 	 1, COL+1, "BEST:", hi);
	printf("\33[%d;%dH%s",		 2, COL+1, "----------");
	printf("\33[%d;%dH%s%d%s%d", 3, COL+1, "SCORE:", score, "/", ROW - 1);
	printf("\33[%d;%dH%s", 		 5, COL+1, "SPEED (Don't cheat!)");
	printf("\33[%d;%dH%s%d%s", 	 6, COL+1, "\33[33m[<]\33[0m  ", speedrate, "%  \33[33m[>]\33[0m");
	printf("\33[%d;%dH%s", ROW/2+3, COL/2-20, "\33[90mPress \33[33m[W]\33[90m, \33[33m[A]\33[90m, \33[33m[S]\33[90m, \33[33m[D]\33[90m to control Froggy.");
	printf("\33[%d;%dH%s", ROW/2+4, COL/2-20, "\33[90mPress \33[33m[Q]\33[90m to quit.\33[0m");
	fflush(stdout);
}

/* Initialize the parameter of the game. */
void init_game() {
	score = 0;
	speedrate = 50;
	init_map();
	frog.jump_to(COL/2, ROW-1);
}

/* Thread: main */
int main(int argc, char *argv[]){
	/* argument y's {1, 2, ..., ROW - 2} passed to threads */
	int y[ROW - 2];
	for (int i = 0; i < ROW - 2; i++)
		y[i] = i + 1;

	/* hide cursor and echo*/
	printf("\033[?25l");
	termios oldt;
    tcgetattr(STDIN_FILENO, &oldt);
    termios newt = oldt;
    newt.c_lflag &= ~ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);

	/* game body */
	bool again = true;
	while (again) {
		init_game();
		choose_difficulty();
		draw_init_screen();
		countdown(3);

		/* Threading begins */
	    pthread_mutex_init(&mutex, NULL);
	    pthread_create(&pth_kb, NULL, check_kb, NULL);
	    for (int i = 0; i < ROW - 2; i++) {
	    	pthread_create(
				&pth_slog[i], NULL, 
				spawn_logs_on_row, (void*) &y[i]);
			pthread_create(
				&pth_dlog[i], NULL,
				drift_logs_on_row, (void*) &y[i]);
		}
		/* wait for kb thread to end */
	    pthread_join(pth_kb, NULL);

		/* cancel all log threads directly */
	    for (int i = 0; i < ROW - 2; i++) {
			pthread_cancel(pth_slog[i]);
			pthread_cancel(pth_dlog[i]);
		}
    	pthread_mutex_destroy(&mutex);

		/*  Display the output for user: win, lose or quit.  */
		switch (status) {
			case 'w': scflush("   ˗ˏˋFROGGY WINS!ˎˊ˗   "); hi = 0; break;
			case 'd': scflush("  FROGGY DIEEEEEED! :/  "); break;
			case 'q': scflush("FROGGY QUITS THE GAME :O"); break;
		}

		/* ask if wannna play again */
		puts("------------------------");
		puts("Play again? \33[33m[Y]\33[0mes / \33[33m[N]\33[0mo");
		while (true) {
			if (kbhit()) {
				char c = getchar();
				if (c == 'y' || c == 'Y') break;
				else if (c == 'n' || c == 'N') {
					again = false; break;
				}
			}
		}
	}

	/* user choosed to exit the game */
	scflush();

	/* restore cursor and echo */
	printf("\033[?25h");
	tcsetattr(STDIN_FILENO, TCSANOW, &oldt);

	return 0;

}
