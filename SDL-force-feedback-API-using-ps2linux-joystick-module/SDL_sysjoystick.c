/*
    SDL - Simple DirectMedia Layer
    Copyright (C) 1997, 1998, 1999, 2000, 2001, 2002  Sam Lantinga
    Copyright (C) 2003  J. Grant  (PS2Linux joystick code and actuator support)

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Library General Public License for more details.

    You should have received a copy of the GNU Library General Public
    License along with this library; if not, write to the Free
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

    Sam Lantinga
    slouken@libsdl.org

    J. Grant 
    jg-sdl@jguk.org
*/

#ifdef SAVE_RCSID
static char rcsid =
 "@(#) $Id: SDL_sysjoystick.c,v 1.12 2003/02/01 20:25:34 slouken Exp $";
#endif


/* This is the system specific header for the SDL joystick API */

#include <stdio.h>		/* For the definition of NULL */
#include <stdlib.h>		/* For getenv() prototype */
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <limits.h>		/* For the definition of PATH_MAX */
#ifdef __arm__
#include <linux/limits.h> /* Arm cross-compiler needs this */
#endif
#include <linux/joystick.h>
#ifdef USE_INPUT_EVENTS
#include <linux/input.h>
#endif

#include <linux/ps2/pad.h>	/* PS2Linux controller defines */

#include "SDL_error.h"
#include "SDL_joystick.h"
#include "SDL_sysjoystick.h"
#include "SDL_joystick_c.h"

/* The maximum number of joysticks we'll detect */
#define MAX_JOYSTICKS	2
#define NUM_BUTTONS 12

/* A list of available joysticks */
static char *SDL_joylist[MAX_JOYSTICKS];

static int ps2padstat_fd;	/* PS2 pad status fd for /dev/ps2padstat */

static int current_axis = -1;	/* Contains the axis number current being sent as SDL_JOYAXISMOTION */

/* The private structure used to keep track of a joystick */
struct joystick_hwdata {
	int fd;
	int joystick_type;		/* Required to know supported features */
	
	/* Required to calculate what has changed and thus SDL_RELEASE joystick events */
	Uint8 old_joystick_buffer[PS2PAD_DATASIZE];
	Uint32 old_joystick_buttons;
	/* The current linux joystick driver maps hats to two axes */
	struct hwdata_hat {
		int axis[2];
	} *hats;
	/* The current linux joystick driver maps balls to two axes */
	struct hwdata_ball {
		int axis[2];
	} *balls;

	/* Support for the Linux 2.4 unified input interface */
#ifdef USE_INPUT_EVENTS
	SDL_bool is_hid;
	Uint8 key_map[KEY_MAX-BTN_MISC];
	Uint8 abs_map[ABS_MAX];
	struct axis_correct {
		int used;
		int coef[3];
	} abs_correct[ABS_MAX];
#endif
};

static char *mystrdup(const char *string)
{
	char *newstring;

	newstring = (char *)malloc(strlen(string)+1);
	if ( newstring ) {
		strcpy(newstring, string);
	}
	return(newstring);
}

#ifdef USE_INPUT_EVENTS
#define test_bit(nr, addr) \
	(((1UL << ((nr) & 31)) & (((const unsigned int *) addr)[(nr) >> 5])) != 0)

static int EV_IsJoystick(int fd)
{
	unsigned long evbit[40];
	unsigned long keybit[40];
	unsigned long absbit[40];

	if ( (ioctl(fd, EVIOCGBIT(0, sizeof(evbit)), evbit) < 0) ||
	     (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybit)), keybit) < 0) ||
	     (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbit)), absbit) < 0) ) {
		return(0);
	}
	if (!(test_bit(EV_KEY, evbit) && test_bit(EV_ABS, evbit) &&
	      test_bit(ABS_X, absbit) && test_bit(ABS_Y, absbit) &&
	     (test_bit(BTN_TRIGGER, keybit) || test_bit(BTN_A, keybit) || test_bit(BTN_1, keybit)))) return 0;
	return(1);
}

#endif /* USE_INPUT_EVENTS */

/* Function to scan the system for joysticks */
int SDL_SYS_JoystickInit(void)
{
	/* The base path of the joystick devices */
	const char *joydev_devices[] = {
#ifdef USE_INPUT_EVENTS
		"/dev/input/event%d",
#endif
		"/dev/ps2pad00",
		"/dev/ps2pad10"
	};
	int numjoysticks;
	int j;
	int fd;
	char path[PATH_MAX];
	dev_t dev_nums[MAX_JOYSTICKS];  /* major/minor device numbers */
	struct stat sb;
	int n, duplicate;

	numjoysticks = 0;

	/* First see if the user specified a joystick to use */
	if ( getenv("SDL_JOYSTICK_DEVICE") != NULL ) {
		strncpy(path, getenv("SDL_JOYSTICK_DEVICE"), sizeof(path));
		path[sizeof(path)-1] = '\0';
		if ( stat(path, &sb) == 0 ) {
			fd = open(path, O_RDONLY, 0);
			if ( fd >= 0 ) {
				/* Assume the user knows what they're doing. */
				SDL_joylist[numjoysticks] = mystrdup(path);
				if ( SDL_joylist[numjoysticks] ) {
					dev_nums[numjoysticks] = sb.st_rdev;
					++numjoysticks;
				}
				close(fd);
			}
		}
	}

	for ( j=0; j < MAX_JOYSTICKS; ++j ) {
		sprintf(path, joydev_devices[j]);

		/* rcg06302000 replaced access(F_OK) call with stat().
		 * stat() will fail if the file doesn't exist, so it's
		 * equivalent behaviour.
		 */
		if ( stat(path, &sb) == 0 ) {
			/* Check to make sure it's not already in list.
			 * This happens when we see a stick via symlink.
			 */
			duplicate = 0;
			for (n=0; (n<numjoysticks) && !duplicate; ++n) {
				if ( sb.st_rdev == dev_nums[n] ) {
					duplicate = 1;
				}
			}
			if (duplicate) {
				continue;
			}
				fd = open(path, O_RDONLY, 0);
			if ( fd < 0 ) {
				continue;
			}

#ifdef USE_INPUT_EVENTS
#ifdef DEBUG_INPUT_EVENTS
			printf("Checking %s\n", path);
#endif
			if ( (i == 0) && ! EV_IsJoystick(fd) ) {
				close(fd);
				continue;
			}
#endif
			close(fd);
				
			/* We're fine, add this joystick */
			SDL_joylist[numjoysticks] = mystrdup(path);
			if ( SDL_joylist[numjoysticks] ) {
				dev_nums[numjoysticks] = sb.st_rdev;
				++numjoysticks;
			}
		} else
			break;
	}

#ifdef USE_INPUT_EVENTS
	/* This is a special case...
	   If the event devices are valid then the joystick devices
	   will be duplicates but without extra information about their
	   hats or balls. Unfortunately, the event devices can't
	   currently be calibrated, so it's a win-lose situation.
	   So : /dev/input/eventX = /dev/input/jsY = /dev/jsY
	*/
	if ( (i == 0) && (numjoysticks > 0) )
		break;
#endif

	ps2padstat_fd = open("/dev/ps2padstat", O_RDONLY | O_NONBLOCK);

	return(numjoysticks);
}

/* Function to get the device-dependent name of a joystick */
const char *SDL_SYS_JoystickName(int index)
{
	char * name;
	char * cat_buffer;

	struct ps2pad_stat joystick_port_status[MAX_JOYSTICKS];
	int joystick_type;

	name = (char *)malloc(128);
	cat_buffer = (char *)malloc(64);

	read(ps2padstat_fd, joystick_port_status, sizeof(joystick_port_status));

	joystick_type = PS2PAD_TYPE(joystick_port_status[index].type);
	
	/* Build up string to return using strcat() to combine */
	sprintf(name, "port %d:  ", joystick_port_status[index].portslot>>4);

	switch(joystick_type)
	{
		case PS2PAD_TYPE_NEJICON:	sprintf(cat_buffer, "Nejicon"); break;

		case PS2PAD_TYPE_DIGITAL: 	sprintf(cat_buffer, "Digital"); break;

		case PS2PAD_TYPE_ANALOG: 	sprintf(cat_buffer, "Analog"); break;

		case PS2PAD_TYPE_DUALSHOCK: 	sprintf(cat_buffer, "DualShock 1/2"); break;

		default: 			sprintf(cat_buffer, "Not connected"); break;
	}
	strcat(name, cat_buffer);
	
	sprintf(cat_buffer, " (type: %d)", joystick_type);
	strcat(name, cat_buffer);

	free(cat_buffer);
	return name;
}

static int allocate_hatdata(SDL_Joystick *joystick)
{
	int i;

	joystick->hwdata->hats = (struct hwdata_hat *)malloc(
		joystick->nhats * sizeof(struct hwdata_hat));
	if ( joystick->hwdata->hats == NULL ) {
		return(-1);
	}
	for ( i=0; i<joystick->nhats; ++i ) {
		joystick->hwdata->hats[i].axis[0] = 1;
		joystick->hwdata->hats[i].axis[1] = 1;
	}
	return(0);
}

static int allocate_balldata(SDL_Joystick *joystick)
{
	int i;

	joystick->hwdata->balls = (struct hwdata_ball *)malloc(
		joystick->nballs * sizeof(struct hwdata_ball));
	if ( joystick->hwdata->balls == NULL ) {
		return(-1);
	}
	for ( i=0; i<joystick->nballs; ++i ) {
		joystick->hwdata->balls[i].axis[0] = 0;
		joystick->hwdata->balls[i].axis[1] = 0;
	}
	return(0);
}

static SDL_bool JS_ConfigJoystick(SDL_Joystick *joystick, int fd)
{
	SDL_bool handled;
	int tmp_naxes, tmp_nhats, tmp_nballs;
	const char *name;
	char *env, env_name[128];
	struct ps2pad_stat joystick_port_status[MAX_JOYSTICKS];
	struct ps2pad_act actuator_align;
	int joystick_type;
	int index;

	handled = SDL_FALSE;
	joystick_type = -1;
	index = joystick->index;

	read(ps2padstat_fd, joystick_port_status, sizeof(joystick_port_status));

	joystick_type = PS2PAD_TYPE(joystick_port_status[index].type);

	switch(joystick_type)
	{
		case PS2PAD_TYPE_DUALSHOCK:
		{
			joystick->naxes = 4;
			joystick->nbuttons = 12;
			joystick->nballs = 0;
			joystick->nhats = 1;
			joystick->nactuators = 2;
			
			joystick->actuators = (struct actuator_info *)
					malloc(joystick->nactuators * sizeof(*joystick->actuators));
			if ( joystick->actuators ) 
			{
				memset(joystick->actuators, 0, joystick->nactuators*sizeof(*joystick->actuators));
			}
			
			/* Describe the actuator propeties */
			joystick->actuators[0].range = 1;
			joystick->actuators[0].type = 0;

			joystick->actuators[1].range = 255;
			joystick->actuators[0].type = 1;

			/* allign actuators */
			memset(&actuator_align, 0xFF, sizeof(actuator_align.data));
			actuator_align.len = 6;
			actuator_align.data[0] = 0;
			actuator_align.data[1] = 1;
			ioctl(joystick->hwdata->fd, PS2PAD_IOCSETACTALIGN, &actuator_align);

			joystick->hwdata->joystick_type = joystick_type;
			handled = SDL_TRUE;
			break;
		}
		case PS2PAD_TYPE_DIGITAL:
		{
			joystick->naxes = 0;
			joystick->nbuttons = 12;
			joystick->nballs = 0;
			joystick->nhats = 1;
			joystick->nactuators = 0;

			joystick->hwdata->joystick_type = joystick_type;
			handled = SDL_TRUE;
			break;
		}
		case PS2PAD_TYPE_ANALOG:
		{
			joystick->naxes = 4;
			joystick->nbuttons = 12;
			joystick->nballs = 0;
			joystick->nhats = 1;
			joystick->nactuators = 0;

			joystick->hwdata->joystick_type = joystick_type;
			handled = SDL_TRUE;
			break;
		}
		/* TODO Specifics of this pad are unknown, using as plain digital */
		case PS2PAD_TYPE_NEJICON:
		{
			joystick->naxes = 0;
			joystick->nbuttons = 12;
			joystick->nballs = 0;
			joystick->nhats = 1;
			joystick->nactuators = 0;

			joystick->hwdata->joystick_type = joystick_type;
			handled = SDL_TRUE;
			break;
		}
		case 6:
			/* Namco G-Con 45 light gun!  SDL support forthcomming? */

		default:
		{
			/* Support currently unknown controlers in plain digital button mode */
			SDL_SetError("Unsupported PS2 pad type %d, trying to use as a plain digital pad\n", joystick_type);
			joystick->naxes = 0;
			joystick->nbuttons = 12;
			joystick->nballs = 0;
			joystick->nhats = 1;
			joystick->nactuators = 0;

			joystick->hwdata->joystick_type = 0;
			handled = SDL_TRUE;
			break;
		}
	}


	name = SDL_SYS_JoystickName(joystick->index);

	/* User environment joystick support */
	if ( (env = getenv("SDL_LINUX_JOYSTICK")) ) {
		strcpy(env_name, "");
		if ( *env == '\'' && sscanf(env, "'%[^']s'", env_name) == 1 )
			env += strlen(env_name)+2;
		else if ( sscanf(env, "%s", env_name) == 1 )
			env += strlen(env_name);

		if ( strcmp(name, env_name) == 0 ) {

			if ( sscanf(env, "%d %d %d", &tmp_naxes, &tmp_nhats,
				&tmp_nballs) == 3 ) {

				joystick->naxes = tmp_naxes;
				joystick->nhats = tmp_nhats;
				joystick->nballs = tmp_nballs;

				handled = SDL_TRUE;
			}
		}
	}

	/* Remap hats and balls */
	if (handled) {
		if ( joystick->nhats > 0 ) {
			if ( allocate_hatdata(joystick) < 0 ) {
				joystick->nhats = 0;
			}
		}
		if ( joystick->nballs > 0 ) {
			if ( allocate_balldata(joystick) < 0 ) {
				joystick->nballs = 0;
			}
		}
	}
	return(handled);
}

#ifdef USE_INPUT_EVENTS

static SDL_bool EV_ConfigJoystick(SDL_Joystick *joystick, int fd)
{
	int i;
	unsigned long keybit[40];
	unsigned long absbit[40];
	unsigned long relbit[40];

	/* See if this device uses the new unified event API */
	if ( (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybit)), keybit) >= 0) &&
	     (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbit)), absbit) >= 0) &&
	     (ioctl(fd, EVIOCGBIT(EV_REL, sizeof(relbit)), relbit) >= 0) ) {
		joystick->hwdata->is_hid = SDL_TRUE;

		/* Get the number of buttons, axes, and other thingamajigs */
		for ( i=BTN_JOYSTICK; i < KEY_MAX; ++i ) {
			if ( test_bit(i, keybit) ) {
#ifdef DEBUG_INPUT_EVENTS
				printf("Joystick has button: 0x%x\n", i);
#endif
				joystick->hwdata->key_map[i-BTN_MISC] =
						joystick->nbuttons;
				++joystick->nbuttons;
			}
		}
		for ( i=BTN_MISC; i < BTN_JOYSTICK; ++i ) {
			if ( test_bit(i, keybit) ) {
#ifdef DEBUG_INPUT_EVENTS
				printf("Joystick has button: 0x%x\n", i);
#endif
				joystick->hwdata->key_map[i-BTN_MISC] =
						joystick->nbuttons;
				++joystick->nbuttons;
			}
		}
		for ( i=0; i<ABS_MAX; ++i ) {
			/* Skip hats */
			if ( i == ABS_HAT0X ) {
				i = ABS_HAT3Y;
				continue;
			}
			if ( test_bit(i, absbit) ) {
				int values[5];

				ioctl(fd, EVIOCGABS(i), values);
#ifdef DEBUG_INPUT_EVENTS
				printf("Joystick has absolute axis: %x\n", i);
				printf("Values = { %d, %d, %d, %d, %d }\n",
					values[0], values[1],
					values[2], values[3], values[4]);
#endif /* DEBUG_INPUT_EVENTS */
				joystick->hwdata->abs_map[i] = joystick->naxes;
				if ( values[1] == values[2] ) {
				    joystick->hwdata->abs_correct[i].used = 0;
				} else {
				    joystick->hwdata->abs_correct[i].used = 1;
				    joystick->hwdata->abs_correct[i].coef[0] =
					(values[2] + values[1]) / 2 - values[4];
				    joystick->hwdata->abs_correct[i].coef[1] =
					(values[2] + values[1]) / 2 + values[4];
				    joystick->hwdata->abs_correct[i].coef[2] =
					(1 << 29) / ((values[2] - values[1]) / 2 - 2 * values[4]);
				}
				++joystick->naxes;
			}
		}
		for ( i=ABS_HAT0X; i <= ABS_HAT3Y; i += 2 ) {
			if ( test_bit(i, absbit) || test_bit(i+1, absbit) ) {
#ifdef DEBUG_INPUT_EVENTS
				printf("Joystick has hat %d\n",(i-ABS_HAT0X)/2);
#endif
				++joystick->nhats;
			}
		}
		if ( test_bit(REL_X, relbit) || test_bit(REL_Y, relbit) ) {
			++joystick->nballs;
		}

		/* Allocate data to keep track of these thingamajigs */
		if ( joystick->nhats > 0 ) {
			if ( allocate_hatdata(joystick) < 0 ) {
				joystick->nhats = 0;
			}
		}
		if ( joystick->nballs > 0 ) {
			if ( allocate_balldata(joystick) < 0 ) {
				joystick->nballs = 0;
			}
		}
	}
	return(joystick->hwdata->is_hid);
}

#endif /* USE_INPUT_EVENTS */

/* Function to open a joystick for use.
   The joystick to open is specified by the index field of the joystick.
   This should fill the nbuttons and naxes fields of the joystick structure.
   It returns 0, or -1 if there is an error.
 */
int SDL_SYS_JoystickOpen(SDL_Joystick *joystick)
{
	int fd;
	int joystick_stat;

	/* Open the joystick and set the joystick file descriptor */
	fd = open(SDL_joylist[joystick->index], O_RDONLY, 0);
	if ( fd < 0 ) {
		SDL_SetError("Unable to open %s\n",
		             SDL_joylist[joystick->index]);
		return(-1);
	}

	/* Check if the joystick is available for use */
	ioctl(fd, PS2PAD_IOCGETSTAT, &joystick_stat);
	switch(joystick_stat)
	{
		case PS2PAD_STAT_NOTCON:
		{
			SDL_SetError("No device connected to %s\n",
		             SDL_joylist[joystick->index]);
			return(-1);
		}
		case PS2PAD_STAT_BUSY:
		{
			/* TODO Possibly wait for a certain time to allow for delays */
			SDL_SetError("Busy device connected to %s\n",
		             SDL_joylist[joystick->index]);
			return(-1);
		}
		case PS2PAD_STAT_READY:
		{
			/* Joystick is ready for action! */
			break;
		}
		case PS2PAD_STAT_ERROR:
		{
			SDL_SetError("Error on device connected to %s\n",
		             SDL_joylist[joystick->index]);
			return(-1);
		}
		default:
		{
			SDL_SetError("Unknown status on device connected to %s\n",
		             SDL_joylist[joystick->index]);
			return(-1);
		}
	}

	joystick->hwdata = (struct joystick_hwdata *)
	                   malloc(sizeof(*joystick->hwdata));
	if ( joystick->hwdata == NULL ) {
		SDL_OutOfMemory();
		close(fd);
		return(-1);
	}
	
	/* Wipe clean the hwdata struct, including joystick buffers */
	memset(joystick->hwdata, 0, sizeof(*joystick->hwdata));

	joystick->hwdata->fd = fd;

	/* Set the joystick to non-blocking read mode */
	fcntl(fd, F_SETFL, O_NONBLOCK);

	/* Get the number of buttons and axes on the joystick */
#ifdef USE_INPUT_EVENTS
	if ( ! EV_ConfigJoystick(joystick, fd) )
#endif
		JS_ConfigJoystick(joystick, fd);

	return(0);
}

static __inline__
void HandleHat(SDL_Joystick *stick, Uint8 hat, int axis, int value)
{
	struct hwdata_hat *the_hat;
	const Uint8 position_map[3][3] = {
		{ SDL_HAT_LEFTUP, SDL_HAT_UP, SDL_HAT_RIGHTUP },
		{ SDL_HAT_LEFT, SDL_HAT_CENTERED, SDL_HAT_RIGHT },
		{ SDL_HAT_LEFTDOWN, SDL_HAT_DOWN, SDL_HAT_RIGHTDOWN }
	};

	the_hat = &stick->hwdata->hats[hat];
	if ( value < 0 ) {
		value = 0;
	} else
	if ( value == 0 ) {
		value = 1;
	} else
	if ( value > 0 ) {
		value = 2;
	}
	if ( value != the_hat->axis[axis] ) {
		the_hat->axis[axis] = value;
		SDL_PrivateJoystickHat(stick, hat,
			position_map[the_hat->axis[1]][the_hat->axis[0]]);
	}
}

static __inline__
void HandleBall(SDL_Joystick *stick, Uint8 ball, int axis, int value)
{
	stick->hwdata->balls[ball].axis[axis] += value;
}

/* Function to update the state of a joystick - called as a device poll.
 * This function shouldn't update the joystick structure directly,
 * but instead should call SDL_PrivateJoystick*() to deliver events
 * and update joystick device state.
 */
static __inline__ void JS_HandleEvents(SDL_Joystick *joystick)
{
	int joystick_stat;
	int joystick_rstat;

	Uint8 joystick_buffer[PS2PAD_DATASIZE];
	Uint32 joystick_buttons;
	Uint32 joystick_buttons_xor;
	int hat_event_temp;
	int button_loop;

	/* Mapping is the same as in Linux Joystick code.  With the exception of L3 and R3
	because these are new numbers (Linux JS code did not support before).  Definition 
	of these 12 PS2 Direction Pad control buttons is used in the xor comparison */
	const int button_index[NUM_BUTTONS] = {
			PS2PAD_BUTTON_SQUARE,
			PS2PAD_BUTTON_CROSS,
			PS2PAD_BUTTON_TRIANGLE,	/* The same as PS2PAD_BUTTON_B */
			PS2PAD_BUTTON_CIRCLE,	/* The same as PS2PAD_BUTTON_A */
			PS2PAD_BUTTON_L1,
			PS2PAD_BUTTON_R1,	/* The same as PS2PAD_BUTTON_R */
			PS2PAD_BUTTON_L2,
			PS2PAD_BUTTON_R2,
			PS2PAD_BUTTON_SELECT,
			PS2PAD_BUTTON_START,
			PS2PAD_BUTTON_L3,
			PS2PAD_BUTTON_R3 };


	joystick_buttons = 0;

	/* Check if the joystick is available for use */
	ioctl(joystick->hwdata->fd, PS2PAD_IOCGETSTAT, &joystick_stat);

	switch(joystick_stat)
	{
		case PS2PAD_STAT_READY:
		{
			/* Connected pad is ready for action! */
			memset(&joystick_buffer, 0, sizeof(joystick_buffer));

			/* Wait until the IO is ready for reading */
			do
			{
				joystick_rstat = PS2PAD_RSTAT_BUSY;
				ioctl(joystick->hwdata->fd, PS2PAD_IOCGETREQSTAT, &joystick_rstat);
			} while (joystick_rstat == PS2PAD_RSTAT_BUSY);

			read(joystick->hwdata->fd, joystick_buffer, sizeof(joystick_buffer));
			joystick_buttons = ~(((unsigned long)joystick_buffer[0] << 24)
				| ((unsigned long)joystick_buffer[1] << 16)
				| ((unsigned long)joystick_buffer[2] << 8)
				| (joystick_buffer[3] << 0));

			/* Evaluate the button states that have changed since the last update.
			   Only send updates for changes in in the joystick state! */
			joystick_buttons_xor = joystick_buttons ^ joystick->hwdata->old_joystick_buttons;

			/* Check if there is a change in the joystick hat (Direction pad) */
			if(joystick_buttons_xor & (PS2PAD_BUTTON_LEFT | PS2PAD_BUTTON_RIGHT | PS2PAD_BUTTON_UP | PS2PAD_BUTTON_DOWN))
			{
				hat_event_temp = SDL_HAT_CENTERED;
				if(joystick_buttons_xor & PS2PAD_BUTTON_LEFT)	hat_event_temp |= SDL_HAT_LEFT;
				if(joystick_buttons_xor & PS2PAD_BUTTON_RIGHT)	hat_event_temp |= SDL_HAT_RIGHT;
				if(joystick_buttons_xor & PS2PAD_BUTTON_UP)	hat_event_temp |= SDL_HAT_UP;
				if(joystick_buttons_xor & PS2PAD_BUTTON_DOWN)	hat_event_temp |= SDL_HAT_DOWN;
				SDL_PrivateJoystickHat(joystick, 0, hat_event_temp);
			}

			/* Check each remaining button and send a button event if it has changed */
			for(button_loop = 0; button_loop < NUM_BUTTONS; button_loop++)
			{
				if(joystick_buttons_xor & button_index[button_loop])
				{
					SDL_PrivateJoystickButton(joystick, button_loop, (joystick_buttons & button_index[button_loop]) ? SDL_PRESSED : SDL_RELEASED);
				}
			}

			joystick->hwdata->old_joystick_buttons = joystick_buttons;

			/* Only send axis events for joysticks that support analog controls! */
			if((joystick->hwdata->joystick_type == PS2PAD_TYPE_DUALSHOCK) ||
				(joystick->hwdata->joystick_type == PS2PAD_TYPE_DUALSHOCK))
			{
				/* Normalise joystick axes into within the -32767 -> +32767 range.
				   
				   The 2 analog sticks (2 axes each) are read in the same order as
				   the Linux JS module. (left == 0,1, right == 2,3).
				   
				   If the axis value has not changed do not send the same value as
				   though there has been a change.
				   
				   When the joystick is first updated by SDL_EventPoll() the
				   old_joystick_buffer is all 0, this has the desired effect of new
				   events for each axis and to be sent
				*/

				/* Do not send axis events when there is no change */
				if(joystick_buffer[6] != joystick->hwdata->old_joystick_buffer[6])
				{
					SDL_PrivateJoystickAxis(joystick, 0, (joystick_buffer[6] << 8) - 32768);
				}

				/* Do not send axis events when there is no change */
				if(joystick_buffer[7] != joystick->hwdata->old_joystick_buffer[7])
				{
					SDL_PrivateJoystickAxis(joystick, 1, (joystick_buffer[7] << 8) - 32768);
				}

				/* Do not send axis events when there is no change */
				if(joystick_buffer[4] != joystick->hwdata->old_joystick_buffer[4])
				{
					SDL_PrivateJoystickAxis(joystick, 2, (joystick_buffer[4] << 8) - 32768);
				}

				/* Do not send axis events when there is no change */
				if(joystick_buffer[5] != joystick->hwdata->old_joystick_buffer[5])
				{
					SDL_PrivateJoystickAxis(joystick, 3, (joystick_buffer[5] << 8) - 32768);
				}
			}
			
			/* Store joystick_buffer for next itteration */
			memcpy(&joystick->hwdata->old_joystick_buffer, &joystick_buffer, sizeof(joystick_buffer));

			break;
		}
		case PS2PAD_STAT_NOTCON:
		{
			SDL_SetError("No device connected to %s\n",
			     SDL_joylist[joystick->index]);
			break;
		}
		case PS2PAD_STAT_BUSY:
		{
			SDL_SetError("Busy device connected to %s\n",
			     SDL_joylist[joystick->index]);
			break;
		}
		case PS2PAD_STAT_ERROR:
		{
			SDL_SetError("Error on device connected to %s\n",
		             SDL_joylist[joystick->index]);
			break;
		}
		default:
		{
			SDL_SetError("Unknown status on device connected to %s\n",
		             SDL_joylist[joystick->index]);
			break;
		}
	}

}


#ifdef USE_INPUT_EVENTS
static __inline__ int EV_AxisCorrect(SDL_Joystick *joystick, int which, int value)
{
	struct axis_correct *correct;

	correct = &joystick->hwdata->abs_correct[which];
	if ( correct->used ) {
		if ( value > correct->coef[0] ) {
			if ( value < correct->coef[1] ) {
				return 0;
			}
			value -= correct->coef[1];
		} else {
			value -= correct->coef[0];
		}
		value *= correct->coef[2];
		value >>= 14;
	}

	/* Clamp and return */
	if ( value < -32767 ) return -32767;
	if ( value >  32767 ) return  32767;

	return value;
}

static __inline__ void EV_HandleEvents(SDL_Joystick *joystick)
{
	struct input_event events[32];
	int i, len;
	int code;

	while ((len=read(joystick->hwdata->fd, events, (sizeof events))) > 0) {
		len /= sizeof(events[0]);
		for ( i=0; i<len; ++i ) {
			code = events[i].code;
			switch (events[i].type) {
			    case EV_KEY:
				if ( code >= BTN_MISC ) {
					code -= BTN_MISC;
					SDL_PrivateJoystickButton(joystick,
				           joystick->hwdata->key_map[code],
					   events[i].value);
				}
				break;
			    case EV_ABS:
				switch (code) {
				    case ABS_HAT0X:
				    case ABS_HAT0Y:
				    case ABS_HAT1X:
				    case ABS_HAT1Y:
				    case ABS_HAT2X:
				    case ABS_HAT2Y:
				    case ABS_HAT3X:
				    case ABS_HAT3Y:
					code -= ABS_HAT0X;
					HandleHat(joystick, code/2, code%2,
							events[i].value);
					break;
				    default:
					events[i].value = EV_AxisCorrect(joystick, code, events[i].value);
					SDL_PrivateJoystickAxis(joystick,
				           joystick->hwdata->abs_map[code],
					   events[i].value);
					break;
				}
				break;
			    case EV_REL:
				switch (code) {
				    case REL_X:
				    case REL_Y:
					code -= REL_X;
					HandleBall(joystick, code/2, code%2,
							events[i].value);
					break;
				    default:
					break;
				}
				break;
			    default:
				break;
			}
		}
	}
}
#endif /* USE_INPUT_EVENTS */

void SDL_SYS_JoystickUpdate(SDL_Joystick *joystick)
{
	int i;

#ifdef USE_INPUT_EVENTS
	if ( joystick->hwdata->is_hid )
		EV_HandleEvents(joystick);
	else
#endif
		JS_HandleEvents(joystick);

	/* Deliver ball motion updates */
	for ( i=0; i<joystick->nballs; ++i ) {
		int xrel, yrel;

		xrel = joystick->hwdata->balls[i].axis[0];
		yrel = joystick->hwdata->balls[i].axis[1];
		if ( xrel || yrel ) {
			joystick->hwdata->balls[i].axis[0] = 0;
			joystick->hwdata->balls[i].axis[1] = 0;
			SDL_PrivateJoystickBall(joystick, (Uint8)i, xrel, yrel);
		}
	}
}

/*
 * Set an actuator value of a joystick
 * The actuator indices start at index 0.
   On the PS2 DualShock 1/2:
	actuator 0 (small) has a boolean frequency
	actuator 1 (big) has 0-255 range
   Both of these are normalised to within the 0-65535 SDL range
 */
int SDL_SYS_JoystickSetActuator(SDL_Joystick *joystick, int actuator, int frequency)
{
	struct ps2pad_act actuator_buffer;
	int normalised_frequency;

	normalised_frequency = 0;

	(joystick->actuators + actuator)->frequency = frequency;

	actuator_buffer.len = 6;
	switch(actuator)
	{
		case 0:
		{
			normalised_frequency = (frequency > 1);
			joystick->actuators[0].normalised = normalised_frequency;
			actuator_buffer.data[0] = normalised_frequency;
			actuator_buffer.data[1] = joystick->actuators[1].normalised;
			break;
		}
		case 1:
		{
			/* Normalise to within PS2 Actuator range for actuator 1 */
			normalised_frequency = frequency >> 8;
			joystick->actuators[1].normalised = normalised_frequency;
			actuator_buffer.data[1] = normalised_frequency;
			actuator_buffer.data[0] = joystick->actuators[0].normalised;
			break;
		}
		default:
		{
			SDL_SetError("Unknown actuator: %d\n", actuator);
			return 1;
		}
	}

	/* printf("\tSDL_SYS_JoystickSetActuator act: %d freq: %d\n", actuator, frequency); */
	ioctl(joystick->hwdata->fd, PS2PAD_IOCSETACT, &actuator_buffer);
	return 0;
}


/* Function to close a joystick after use */
void SDL_SYS_JoystickClose(SDL_Joystick *joystick)
{
	int loop;

	/* If joystick has actuators ensure they are off */
	for(loop = 0; loop < joystick->nactuators; loop++) {
		SDL_JoystickSetActuator(joystick, loop, 0);
	}

	if ( joystick->hwdata ) {
		close(joystick->hwdata->fd);
		if ( joystick->hwdata->hats ) {
			free(joystick->hwdata->hats);
		}
		if ( joystick->hwdata->balls ) {
			free(joystick->hwdata->balls);
		}
		free(joystick->hwdata);
		joystick->hwdata = NULL;
	}
}

/* Function to perform any system-specific joystick related cleanup */
void SDL_SYS_JoystickQuit(void)
{
	int i;
	for ( i=0; (SDL_joylist[i] && i < MAX_JOYSTICKS); ++i ) {
		free(SDL_joylist[i]);
		SDL_joylist[i] = NULL;
	}

	SDL_joylist[0] = NULL;
}

