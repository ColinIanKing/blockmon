/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Copyright (C) Colin Ian King 2015-2016
 * colin.i.king@gmail.com
 */
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <stdint.h>
#include <libgen.h>
#include <inttypes.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <fcntl.h>
#include <signal.h>
#include <ncurses.h>
#include <dirent.h>
#include <libgen.h>
#include <ctype.h>
#include <setjmp.h>
#include <pthread.h>

#define APP_NAME "BlockMon"

#define BLK_HASH_SIZE		(10007)

#define MAXIMUM(a, b)		((a) > (b) ? (a) : (b))
#define MINIMUM(a, b)		((a) < (b) ? (a) : (b))

#define DEFAULT_UDELAY		(15000)	/* Delay between each refresh */
#define BLINK_MASK		(0x20)	/* Cursor blink counter mask */


/*
 *  Memory size scaling
 */
#define KB			(1024ULL)
#define MB			(KB * KB)
#define GB			(KB * KB * KB)
#define TB			(KB * KB * KB * KB)

/*
 *  Error returns
 */
#define OK			(0)
#define ERR_FAULT		(-1)
#define ERR_RESIZE_FAIL		(-2)
#define ERR_SMALL_WIN		(-3)

#define MAJOR(dev)		((dev) >> 8)
#define MINOR(dev)		((dev) & 0xff)

enum {
	WHITE_RED = 1,
	WHITE_BLUE,
	WHITE_YELLOW,
	WHITE_CYAN,
	WHITE_GREEN,
	WHITE_BLACK,
	BLUE_CYAN,
	RED_BLUE,
	BLACK_WHITE,
	BLACK_BLACK,
	BLUE_WHITE,
};

static int colours[] = {
	BLUE_CYAN,
	WHITE_GREEN,
	WHITE_YELLOW,
	WHITE_RED,
};

/*
 *  Note that we use 64 bit addresses even for 32 bit systems since
 *  this allows blockmon to run in a 32 bit chroot and still access
 *  the 64 bit mapping info.  Otherwise I'd use uintptr_t instead.
 */
typedef uint64_t blkaddr_t;		/* Addresses */
typedef int64_t index_t;		/* Index into page tables */

typedef struct blk {
	struct blk	*list_next;
	struct blk	*hash_next;
	blkaddr_t	addr;
	uint16_t	count;
} blk_t;

/*
 *  Cursor context, we have one each for the
 *  memory map and page contents views
 */
typedef struct {
	int32_t xpos;			/* Cursor x position */
	int32_t ypos;			/* Cursor y position */
	int32_t xpos_prev;		/* Previous x position */
	int32_t ypos_prev;		/* Previous y position */
	int32_t ypos_max;		/* Max y position */
	int32_t xmax;			/* Width */
	int32_t ymax;			/* Height */
} position_t;

/*
 *  Globals, stashed in a global struct
 */
typedef struct {
	WINDOW *mainwin;		/* curses main window */
	uint32_t  blksize;		/* size of a block */
	blkaddr_t nblocks;		/* number of blocks */
	sigjmp_buf env;			/* terminate abort jmp */
	bool curses_started;		/* Are we in curses mode? */
	bool help_view;			/* Help pop-up info */
	bool resized;			/* SIGWINCH occurred */
	bool terminate;			/* SIGSEGV termination */
	uint8_t opt_flags;		/* User option flags */
	blk_t *blk_hash[BLK_HASH_SIZE];	/* hash table of blks */
	blk_t *blk_list;		/* list of blks */
	pthread_mutex_t mutex;		/* */
	dev_t dev;			/* Device */
	FILE *fp;			/* event pipe */
} global_t;

static global_t g;

/*
 *  banner()
 *	clear a banner across the window
 */
static inline void banner(const int y)
{
	mvwprintw(g.mainwin, y, 0, "%*s", COLS, "");
}


static inline size_t blk_hash(const blkaddr_t addr)
{
	return (size_t)(addr % BLK_HASH_SIZE);
}

static inline blk_t *blk_find(const blkaddr_t addr)
{
	size_t h = blk_hash(addr);
	blk_t *blk;

	pthread_mutex_lock(&g.mutex);
	for (blk = g.blk_hash[h]; blk; blk = blk->hash_next) {
		if (blk->addr == addr) {
			pthread_mutex_unlock(&g.mutex);
			return blk;
		}
	}
	pthread_mutex_unlock(&g.mutex);

	return NULL;
}

static blk_t *blk_new(blkaddr_t addr)
{
	blk_t *blk;
	size_t h = blk_hash(addr);

	blk = malloc(sizeof(*blk));
	if (!blk)
		return NULL;

	memset(blk, 0, sizeof(*blk));

	pthread_mutex_lock(&g.mutex);
	blk->addr = addr;
	blk->list_next = g.blk_list;
	g.blk_list = blk;

	blk->hash_next = g.blk_hash[h];
	g.blk_hash[h] = blk;
	pthread_mutex_unlock(&g.mutex);

	return blk;
}

static void blk_hash_unlink(blk_t *blk_old)
{
	size_t h = blk_hash(blk_old->addr);
	blk_t *blk, *blk_prev = NULL;

	pthread_mutex_lock(&g.mutex);
	blk = g.blk_hash[h];

	while (blk) {
		blk_t *blk_hash_next = blk->hash_next;

		if (blk == blk_old) {
			if (blk_prev == NULL) {
				g.blk_hash[h] = blk_hash_next;
				break;
			} else {
				blk_prev->hash_next = blk_hash_next;
				break;
			}
		} else {
			blk_prev = blk;
		}
		blk = blk_hash_next;
	}
	pthread_mutex_unlock(&g.mutex);
}

static blk_t *blk_inc_count(const blkaddr_t addr)
{
	blk_t *blk = blk_find(addr);

	if (!blk) {
		blk = blk_new(addr);
		if (!blk)
			return NULL;
	}
	pthread_mutex_lock(&g.mutex);
	blk->count = (blk->count < 0x00ff) ?
		blk->count + 64 : 0x00ff;
	pthread_mutex_unlock(&g.mutex);

	return blk;
}

static void blks_age(void)
{
	blk_t *blk = g.blk_list;
	blk_t *blk_prev = NULL;

	while (blk) {
		blk_t *blk_list_next = blk->list_next;

		blk->count--;
		if (blk->count == 0) {
			blk_hash_unlink(blk);

			if (blk_prev == NULL) {
				g.blk_list = blk_list_next;
			} else {
				blk_prev->list_next = blk_list_next;
			}
			free(blk);
		} else {
			blk_prev = blk;
		}
		blk = blk_list_next;
	}
}


static void blks_dump(position_t *position)
{
	blk_t *blk = g.blk_list;
	const size_t nblocks = position->xmax * position->ymax;
	double blks_per_map = (float)g.nblocks / (float)nblocks;
	double  map[nblocks];
	int x, y;
	size_t i;
	float max = 0.0;

	for (i = 0; i < nblocks; i++)
		map[i] = 0.0;

	while (blk) {
		size_t posn = (size_t)((double)blk->addr / blks_per_map);
		if (posn < nblocks) {
			map[posn] += (float)blk->count;
			if (map[posn] > max)
				max = map[posn];
		}
		blk = blk->list_next;
	}

	wattrset(g.mainwin, COLOR_PAIR(WHITE_BLUE));
	for (i = 0, y = 0; y < position->ymax; y++) {
		for (x = 0; x < position->xmax; x++) {
			float v = map[i] / blks_per_map;
			uint16_t val = (uint16_t)v;
			int colour = 0;
			if (v > 0.0) {
				val = 1;
				colour = 1;
			}
			if (colour > 3) colour = 3;
			if (colour < 0) colour = 0;
			wattrset(g.mainwin, COLOR_PAIR(colours[colour]));
			mvwprintw(g.mainwin, y + 1, x, "%c",
				val == 0 ? '-' : ' ');
			i++;
		}
	}

	banner(LINES - 1);
	wattrset(g.mainwin, COLOR_PAIR(WHITE_BLUE) | A_BOLD);
	mvwprintw(g.mainwin, LINES - 1, 0, "1 square = %.1f blocks",
		blks_per_map);
}

static void *reader_thread(void *arg)
{
	(void)arg;
	char buffer[512];

	/* jbd2/sda3-8-629   [001] .... 64723.852280: block_dirty_buffer: 8,3 sector=38797433 size=4096 */

	while (fgets(buffer, sizeof(buffer), g.fp)) {
		char func[128];
		unsigned int major, minor;
		uint64_t sector;

		sscanf(buffer, "%*s %*s %*s %*f: %s %d,%d sector=%" SCNu64, 
			func, &major, &minor, &sector);

		if (major == MAJOR(g.dev) && minor == MINOR(g.dev)) {
			blk_inc_count(sector);
		}
	}
	g.terminate = true;

	return NULL;
}


/*
 *  handle_winch()
 *	handle SIGWINCH, flag a window resize
 */
static void handle_winch(int sig)
{
	(void)sig;

	g.resized = true;
}

/*
 *  handle_terminate()
 *	handle termination signals
 */
static void handle_terminate(int sig)
{
	static bool already_handled = false;
	(void)sig;

	if (already_handled) {
		exit(EXIT_FAILURE);
	}

	g.terminate = true;

	siglongjmp(g.env, 1);
}

/*
 *  show_usage()
 *	mini help info
 */
static void show_usage(void)
{
	printf(APP_NAME ", version " VERSION "\n\n"
		"Usage: " APP_NAME " [options]\n"
		" -h        help\n"
		" -p pid    process ID to monitor\n"
		" -r        read (page back in) pages at start\n"
		" -v        enable VM view\n");
}

/*
 *  show_help()
 *	show pop-up help info
 */
static inline void show_help(void)
{
	const int x = (COLS - 45) / 2;
	int y = (LINES - 15) / 2;

	wattrset(g.mainwin, COLOR_PAIR(WHITE_RED) | A_BOLD);
	mvwprintw(g.mainwin, y++,  x,
		" Blockmon Process Memory Monitor Quick Help ");
	mvwprintw(g.mainwin, y++,  x,
		"%43s", "");
	mvwprintw(g.mainwin, y++,  x,
		" ? or h     This help information%10s", "");
	mvwprintw(g.mainwin, y++,  x,
		" Esc or q   Quit%27s", "");
	mvwprintw(g.mainwin, y++,  x,
		" Enter      Toggle map/memory views%8s", "");
	mvwprintw(g.mainwin, y++,  x,
		" PgUp/Down  Scroll up/down 1/2 page%8s", "");
	mvwprintw(g.mainwin, y++, x,
		" Home/End   Move cursor back to top/bottom ");
	mvwprintw(g.mainwin, y, x,
		" Cursor keys move Up/Down/Left/Right%7s", "");
}

/*
 *  update_xymax()
 *	set the xymax scale for a specific view v
 *	based on column width and scaling factor for
 *	page or mem (hex) views
 */
static inline void update_xymax(position_t *position)
{
	position->xmax = COLS - 1;
	position->ymax = LINES - 2;
}

/*
 *  reset_cursor()
 *	reset to home position
 */
static inline void reset_cursor(
	position_t *p,
	index_t *data_index)
{
	p->xpos = 0;
	p->ypos = 0;
	*data_index = 0;
}

static inline int get_fs_info(const char *path)
{
	struct stat buf;
	struct statvfs vfsbuf;

	if (stat(path, &buf) < 0)
		return -errno;

	g.dev = buf.st_dev;

	if (statvfs(path, &vfsbuf) < 0)
		return -errno;

	g.blksize = vfsbuf.f_bsize;
	g.nblocks = vfsbuf.f_blocks;

	return 0;
}

int main(int argc, char **argv)
{
	struct sigaction action;
	useconds_t udelay;
	position_t position;
	index_t data_index, prev_data_index;
	int32_t blink;
	int rc, ret;
	pthread_t reader;

	if (sigsetjmp(g.env, 0)) {
		rc = ERR_FAULT;
		goto terminate;
	}

	rc = OK;
	blink = 0;
	udelay = DEFAULT_UDELAY;
	data_index = 0;

	for (;;) {
		int c = getopt(argc, argv, "ad:hp:rt:vz:");

		if (c == -1)
			break;
		switch (c) {
		case 'd':
			udelay = strtoul(optarg, NULL, 10);
			if (errno) {
				fprintf(stderr, "Invalid delay value\n");
				exit(EXIT_FAILURE);
			}
			break;
		case 'h':
			show_usage();
			exit(EXIT_SUCCESS);
			break;
		default:
			show_usage();
			exit(EXIT_FAILURE);
		}
	}
	if (geteuid() != 0) {
		fprintf(stderr, APP_NAME " requires root privileges\n");
		exit(EXIT_FAILURE);
	}
	if (get_fs_info(".") < 0) {
		fprintf(stderr, "cat stat file system\n");
		exit(EXIT_FAILURE);
	}

	pthread_mutex_init(&g.mutex, NULL);

	g.fp = fopen("/sys/kernel/debug/tracing/trace_pipe", "r");
	if (!g.fp) {
		fprintf(stderr, "cannot open trace pipe\n");
		exit(EXIT_FAILURE);
	}
	if (pthread_create(&reader, NULL, reader_thread, NULL) < 0) {
		(void)fclose(g.fp);
		fprintf(stderr, "reader thread failed\n");
		exit(EXIT_FAILURE);
		
	}

	memset(&action, 0, sizeof(action));
	action.sa_handler = handle_winch;
	if (sigaction(SIGWINCH, &action, NULL) < 0) {
		fprintf(stderr, "Could not set up window resizing handler\n");
		exit(EXIT_FAILURE);
	}
	memset(&action, 0, sizeof(action));
	action.sa_handler = handle_terminate;
	if (sigaction(SIGSEGV, &action, NULL) < 0) {
		fprintf(stderr, "Could not set up error handler\n");
		exit(EXIT_FAILURE);
	}
	if (sigaction(SIGBUS, &action, NULL) < 0) {
		fprintf(stderr, "Could not set up error handler\n");
		exit(EXIT_FAILURE);
	}

	g.nblocks = 191246996;

	initscr();
	start_color();
	cbreak();
	noecho();
	nodelay(stdscr, 1);
	keypad(stdscr, 1);
	curs_set(0);
	g.mainwin = newwin(LINES, COLS, 0, 0);
	g.curses_started = true;

	init_pair(WHITE_RED, COLOR_WHITE, COLOR_RED);
	init_pair(WHITE_BLUE, COLOR_WHITE, COLOR_BLUE);
	init_pair(WHITE_YELLOW, COLOR_WHITE, COLOR_YELLOW);
	init_pair(WHITE_CYAN, COLOR_WHITE, COLOR_CYAN);
	init_pair(WHITE_GREEN, COLOR_WHITE, COLOR_GREEN);
	init_pair(WHITE_BLACK, COLOR_WHITE, COLOR_BLACK);
	init_pair(BLUE_CYAN, COLOR_BLUE, COLOR_CYAN);
	init_pair(BLACK_WHITE, COLOR_BLACK, COLOR_WHITE);
	init_pair(RED_BLUE, COLOR_RED, COLOR_BLUE);
	init_pair(BLACK_BLACK, COLOR_BLACK, COLOR_BLACK);
	init_pair(BLUE_WHITE, COLOR_BLUE, COLOR_WHITE);

	memset(&position, 0, sizeof(position));
	update_xymax(&position);

	for (;;) {
		int ch, blink_attrs;
		char cursor_ch;
		float percent;


		/*
		 *  SIGWINCH window resize triggered so
		 *  handle window resizing in ugly way
		 */
		if (g.resized) {
			int newx, newy;
			const index_t cursor_index = data_index +
				(position.xpos + (position.ypos * position.xmax));
			struct winsize ws;

			if (ioctl(fileno(stdin), TIOCGWINSZ, &ws) < 0) {
				rc = ERR_RESIZE_FAIL;
				break;
			}
			newy = ws.ws_row;
			newx = ws.ws_col;

			/* Way too small, give up */
			if ((COLS < 23) || (LINES < 1)) {
				rc = ERR_SMALL_WIN;
				break;
			}

			resizeterm(newy, newx);
			wresize(g.mainwin, newy, newx);
			wrefresh(g.mainwin);
			refresh();

			wbkgd(g.mainwin, COLOR_PAIR(RED_BLUE));
			g.resized = false;
			position.xpos = 0;
			position.ypos = 0;
			data_index = cursor_index;
		}

		/*
		 *  Window getting too small, tell user
		 */
		if ((COLS < 80) || (LINES < 23)) {
			wclear(g.mainwin);
			wbkgd(g.mainwin, COLOR_PAIR(RED_BLUE));
			wattrset(g.mainwin, COLOR_PAIR(WHITE_RED) | A_BOLD);
			mvwprintw(g.mainwin, LINES / 2, (COLS / 2) - 8,
				" WINDOW TOO SMALL ");
			wrefresh(g.mainwin);
			refresh();
			usleep(udelay);
			continue;
		}

		update_xymax(&position);
		wbkgd(g.mainwin, COLOR_PAIR(RED_BLUE));

		blink++;
		{
			int32_t curxpos = position.xpos;
			const position_t *pc = &position;
			const index_t cursor_index = data_index +
				(pc->xpos + (pc->ypos * pc->xmax));
			percent = (g.nblocks > 0) ?
				100.0 * cursor_index / g.nblocks : 100;

			/* Memory may have shrunk, so check this */
			if (cursor_index >= (index_t)g.nblocks) {
				/* Force end of memory key action */
				ch = KEY_END;
				goto force_ch;
			}

			blink_attrs = A_BOLD | ((blink & BLINK_MASK) ?
				COLOR_PAIR(WHITE_BLUE) :
				COLOR_PAIR(BLUE_WHITE));
			wattrset(g.mainwin, blink_attrs);
			cursor_ch = mvwinch(g.mainwin, position.ypos + 1, curxpos)
				& A_CHARTEXT;
			mvwprintw(g.mainwin, position.ypos + 1, curxpos,
				"%c", cursor_ch);
			blink_attrs = A_BOLD | ((blink & BLINK_MASK) ?
				COLOR_PAIR(BLACK_WHITE) :
				COLOR_PAIR(WHITE_BLACK));
		}
		ch = getch();

		if (g.help_view)
			show_help();

		wattrset(g.mainwin, COLOR_PAIR(WHITE_BLUE) | A_BOLD);
		banner(0);

		mvwprintw(g.mainwin, 0, 0, "Blockmon: %" PRIu64 " blocks @ "
			"%" PRIu32 " bytes per block",
			g.nblocks, g.blksize);
		mvwprintw(g.mainwin, 0, COLS - 8, " %6.1f%%", percent);

		blks_dump(&position);
		blks_age();

		wrefresh(g.mainwin);
		refresh();
force_ch:
		prev_data_index = data_index;
		position.xpos_prev = position.xpos;
		position.ypos_prev = position.ypos;

		switch (ch) {
		case 27:	/* ESC */
		case 'q':
		case 'Q':
			/* Quit */
			g.terminate = true;
			break;
		case 'h':
			/* Toggle Help */
			g.help_view = !g.help_view;
			break;
		case 'c':
		case 'C':
			/* Clear pop ups */
			g.help_view = false;
			break;
		case KEY_DOWN:
			blink = 0;
			position.ypos++;
			break;
		case KEY_UP:
			blink = 0;
			position.ypos--;
			break;
		case KEY_LEFT:
			blink = 0;
			position.xpos--;
			break;
		case KEY_RIGHT:
			blink = 0;
			position.xpos++;
			break;
		case KEY_NPAGE:
			blink = 0;
			position.ypos += position.ymax / 2;
			break;
		case KEY_PPAGE:
			blink = 0;
			position.ypos -= position.ymax / 2;
			break;
		case KEY_HOME:
			reset_cursor(&position, &data_index);
			break;
		case KEY_END:
			break;
		}

		position.ypos_max = position.ymax;

		if (position.xpos >= position.xmax) {
			position.xpos = 0;
			position.ypos++;
		}
		if (position.xpos < 0) {
			position.xpos = position.xmax - 1;
			position.ypos--;
		}

		/*
		 *  Handling yposition overflow / underflow
		 *  is non-trivial as we need to consider
		 *  different views and how to handle the
		 *  scroll data and page index offsets
		 */
		if (position.ypos > position.ymax - 1) {
			data_index += position.xmax *
				(position.ypos - (position.ymax - 1));
			position.ypos = position.ymax - 1;
		}
		if (position.ypos < 0) {
			data_index -= position.xmax * (-position.ypos);
			position.ypos = 0;
			if (data_index < 0) {
				data_index = 0;
			}
		}
		if (data_index < 0) {
			data_index = 0;
			position.ypos = 0;
		}
		{
		const position_t *pc = &position;
		const index_t cursor_index = data_index +
			(pc->xpos + (pc->ypos * pc->xmax));
		const blkaddr_t addr =
			(cursor_index >= (index_t)g.nblocks) ?
				g.nblocks :
				(blkaddr_t)data_index + (position.xpos + (position.ypos * position.xmax));

		if (addr >= g.nblocks) {
			data_index = prev_data_index;
			position.xpos = position.xpos_prev;
			position.ypos = position.ypos_prev;
		}
		}
		if (g.terminate)
			break;

		usleep(udelay);
	}

	werase(g.mainwin);
	wrefresh(g.mainwin);
	refresh();
	delwin(g.mainwin);

terminate:
	//(void)pthread_join(reader, NULL);
	(void)fclose(g.fp);
	if (g.curses_started) {
		clear();
		endwin();
	}

	ret = EXIT_FAILURE;
	switch (rc) {
	case OK:
		ret = EXIT_SUCCESS;
		break;
	case ERR_FAULT:
		fprintf(stderr, "Segfault!\n");
		break;
	default:
		fprintf(stderr, "Unknown failure (%d)\n", rc);
		break;
	}
	exit(ret);
}
