// created by Andreas Schmid, 2019
// this software is released into the public domain, do whatever you want with it
//
// Usage: latency_daemon [event_handle] [min_delay_move] [max_delay_move] [fifo_path]
// event_handle: path to input device you want to delay (e.g. /dev/input/event5)
// min_delay_click: minimum delay to be added to click events (in milliseconds)
// max_delay_click: maximum delay to be added to click events (in milliseconds)
// min_delay_move: minimum delay to be added to mouse movement (in milliseconds)
// max_delay_move: maximum delay to be added to mouse movement (in milliseconds)
// fifo_path: path to a FIFO used to remotely set delay times during runtime (optional)
// Use the same value for min and max to achieve constant delays.

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h> 
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <signal.h>
#include <math.h>
#include <libevdev/libevdev.h>
#include <linux/rtc.h>

// set to 1 for more verbose console output
#define DEBUG 1

typedef struct
{
    int fd;                     // file descriptor of the input device
    int type;                   // event type (e.g. key press, relative movement, ...)
    int code;                   // event code (e.g. for key pressses the key/button code)
    int value;                  // event value (e.g. 0/1 for button up/down, coordinates for absolute movement, ...)
    long delay;                  // delay time for the event in milliseconds
    unsigned long timestamp;    // time the event occured
} delayed_event;

typedef struct
{
    size_t size;
    size_t used;
    delayed_event* events;
} event_vector;

event_vector ev;     // vector of all input events
int fd_event = -1;   // actual input device
int virtual_fd = -1; // virtual device for delayed events
int fifo_fd = -1;    // path to FIFO for remotely controlled delay times

char* event_handle; // event handle of the input event we want to add delay to (normally somewhere in /dev/input/)

const char* log_file = "event_log.csv";
char* fifo_path;
pthread_t fifo_thread; 

// use attributes to create threads in a detached state
pthread_attr_t invoked_event_thread_attr, log_delay_val_thread_attr;

enum{
    linear,
    normal
} distribution;

// normal distribution variables
double mu = -1.0;
double sigma = -1.0;

// delay range for mouse clicks
int min_delay_click = -1;
int max_delay_click = -1;

// delay range for mouse movement
// note that variance here causes the movement to stutter
int min_delay_move = -1;
int max_delay_move = -1;

struct libevdev *event_dev = NULL;
struct libevdev *uinput_dev = NULL;
int polling_rate = 8192;

// https://stackoverflow.com/a/3536261
void init_vector(event_vector *ev, size_t size)
{
    ev->events = malloc(size * sizeof(delayed_event));
    ev->used = 0;
    ev->size = size;
}

void append_to_vector(event_vector *ev, delayed_event event)
{
    // upgrade allocated memory if necessary
    if(ev->used >= ev->size)
    {
        ev->size *= 2;
        ev->events = realloc(ev->events, ev->size * sizeof(delayed_event));
    }
    ev->events[ev->used++] = event;
}

void free_vector(event_vector *ev)
{
    free(ev->events);
    ev->events = NULL;
    ev->used = ev->size = 0;
}

void write_event_log(event_vector *ev)
{
    // write header if file doesn't exist
    if(access(log_file, F_OK) != 0)
    {
        FILE *file = fopen(log_file, "w+");
        const char* header = "timestamp;delay;type;value;code\n";
        fwrite(header, 1, strlen(header), file);
        fclose(file);
    }

    FILE *file = fopen(log_file, "a");
    for(int i=0; i<ev->used; ++i)
    {
        delayed_event evnt = ev->events[i];
        fprintf(file,
                "%lu;%li;%i;%i;%i\n",
                evnt.timestamp,
                evnt.delay,
                evnt.type,
                evnt.value,
                evnt.code);
    }
    fclose(file);
    free_vector(ev);
}

// returns a normally distributed value around an average mu with std sigma
// source: https://phoxis.org/2013/05/04/generating-random-numbers-from-normal-distribution-in-c/
int randn (double mu, double sigma)
{
  double U1, U2, W, mult;
  static double X1, X2;
  static int call = 0;
 
  if (call == 1)
    {
      call = !call;
      return (mu + sigma * (double) X2);
    }
 
  do
    {
      U1 = -1 + ((double) rand () / RAND_MAX) * 2;
      U2 = -1 + ((double) rand () / RAND_MAX) * 2;
      W = pow (U1, 2) + pow (U2, 2);
    }
  while (W >= 1 || W == 0);
 
  mult = sqrt ((-2 * log (W)) / W);
  X1 = U1 * mult;
  X2 = U2 * mult;
 
  call = !call;
 
  return (mu + sigma * (double) X1);
}

// generate a delay time for an input event
// this function uses a linear distribution between min_delay_move and max_delay_move
// other distributions (e.g. gaussian) may be added in the future
int calculate_delay(int min, int max)
{
    if(min == max) return min; // add constant delay if no range is specified
    else if(distribution == linear) return min + (rand() % (max - min));
    else if(distribution == normal)
    {
        int x = -1;
        while(x < min || x > max)
        {
            x = randn(mu, sigma);
        }
        printf("%d\n", x);
        return x;
    }
    else return 0;
}

// creates an input event for the specified device
// source: https://www.kernel.org/doc/html/v4.12/input/uinput.html
void emit(int fd, int type, int code, int val)
{
    struct input_event ie;
    
    ie.type = type;
    ie.code = code;
    ie.value = val;
    
    ie.time.tv_sec = 0;
    ie.time.tv_usec = 0;
    
    write(fd, &ie, sizeof(ie));
}

// wait for some time, then emit an input event to a virtual input device
void *invoke_delayed_event(void *args) 
{ 
    delayed_event *event = args;

    int eventFd = event->fd;
    int eventType = event->type;
    int eventCode = event->code;
    int eventValue = event->value;
    int eventDelay = event->delay;

    usleep(eventDelay * 1000); // wait for the specified delay time (in milliseconds)

    emit(eventFd, eventType, eventCode, eventValue); // this is the actual delayed input event (eg. mouse move or click)
    emit(eventFd, EV_SYN, SYN_REPORT, 0); // EV_SYN events have to come in time so we trigger them manually

    pthread_exit(NULL);
}

void *invoke_delayed_evdev_event(void *args) 
{ 
    delayed_event *event = args;
    int eventDelay = event->delay;

    usleep(eventDelay * 1000); // wait for the specified delay time (in milliseconds)

    int rc = libevdev_uinput_write_event(
            uinput_dev, event->type,
            event->code, event->value);

    if (rc != 0) {
        printf("Failed to write uinput event: %s\n",
                    strerror(-rc));
    }

    rc = libevdev_uinput_write_event(uinput_dev, EV_SYN, SYN_REPORT, 0);

    // emit(eventFd, eventType, eventCode, eventValue); // this is the actual delayed input event (eg. mouse move or click)
    // emit(eventFd, EV_SYN, SYN_REPORT, 0); // EV_SYN events have to come in time so we trigger them manually

    pthread_exit(NULL);
}

int get_event(struct input_event *event)
{
    struct timeval current_time;
	gettimeofday(&current_time, NULL);
    // if(timercmp(&current_time, &event->time, <)) return -1;

	int rc = LIBEVDEV_READ_STATUS_SUCCESS;

    rc = libevdev_next_event(event_dev,
                    LIBEVDEV_READ_FLAG_NORMAL |
                    LIBEVDEV_READ_FLAG_BLOCKING, event);

    /* Handle dropped SYN. */
    if (rc == LIBEVDEV_READ_STATUS_SYNC) {
        printf("Warning, syn dropped: (%d) %s\n", -rc, strerror(-rc));

        while (rc == LIBEVDEV_READ_STATUS_SYNC) {
            rc = libevdev_next_event(event_dev,
                    LIBEVDEV_READ_FLAG_SYNC, event);
        }
    }

	if (rc == -ENODEV)
    {
		printf("Device disconnected: (%d) %s\n", -rc, strerror(-rc));
        return -1;
	}
    return 1;
}

// thread to handle external modification of delay times using a FIFO
void *handle_fifo(void *args)
{
    char buffer[80];

    // needed so we don't lose our old delay times in case something goes wrong
    int buffer_min_delay_click, buffer_max_delay_click, buffer_min_delay_move, buffer_max_delay_move;

    while(1)
    {
        // open the FIFO - this call blocks the thread until someone writes to the FIFO
        fifo_fd = open(fifo_path, O_RDONLY);

        if(read(fifo_fd, buffer, 80) <= 0) continue; // read the FIFO's content into a buffer and skip setting the variables if an error occurs

        // parse new values from the FIFO
        // only set the delay times if all four values could be read correctly
        if(sscanf(buffer, "%d %d %d %d", &buffer_min_delay_click, &buffer_max_delay_click, &buffer_min_delay_move, &buffer_max_delay_move) == 4)
        {
            // set delay times
            min_delay_click = buffer_min_delay_click;
            max_delay_click = buffer_max_delay_click;
            min_delay_move = buffer_min_delay_move;
            max_delay_move = buffer_max_delay_move;

            // make sure max >= min
            if(max_delay_click < min_delay_click) max_delay_click = min_delay_click;
            if(max_delay_move < min_delay_move) max_delay_move = min_delay_move;

            if(DEBUG) printf("set new values: %d %d %d %d\n", min_delay_click, max_delay_click, min_delay_move, max_delay_move);
        }
        else
        {
            if(DEBUG) printf("could not set new delays - bad data structure\n");
        }

        close(fifo_fd);
    }
}

// create a FIFO for inter process communication at the path defined by the 6th command line parameter (recommended: somewhere in /tmp)
// this can be used to adjust the delay values with an external program during runtime
// simply write (or echo) four numbers (min_delay_click max_delay_click min_delay_move max_delay move) separated by whitespaces into the FIFO
int init_fifo()
{
    unlink(fifo_path); // unlink the FIFO if it already exists
    umask(0); // needed for permissions, I have no idea what this exactly does
    if(mkfifo(fifo_path, 0666) == -1) return 0; // create the FIFO

    // create a thread reading the FIFO and adjusting the delay times
    pthread_create(&fifo_thread, NULL, handle_fifo, NULL); 

    return 1;
}

// enable mouse buttons and relative events
// possible events of input devices can be found using the program evtest
// the meaning of those key codes can be found here: https://www.kernel.org/doc/html/v4.15/input/event-codes.html
void enable_mouse_events(int virtual_fd)
{
    ioctl(virtual_fd, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(virtual_fd, UI_SET_KEYBIT, KEY_SPACE);
    ioctl(virtual_fd, UI_SET_KEYBIT, BTN_RIGHT);

    ioctl(virtual_fd, UI_SET_EVBIT, EV_REL);
    ioctl(virtual_fd, UI_SET_RELBIT, REL_X);
    ioctl(virtual_fd, UI_SET_RELBIT, REL_Y);
    ioctl(virtual_fd, UI_SET_RELBIT, REL_WHEEL);
}

//enable all keys on most keyboards
void enable_keyboard_events(int virtual_fd)
{
    for(int keycode=1; keycode<=200; ++keycode)
    {
        ioctl(virtual_fd, UI_SET_KEYBIT, keycode);
    }
}

// create a virtual input device
// this device is used to trigger delayed input events
// source: https://www.kernel.org/doc/html/v4.12/input/uinput.html
int init_virtual_input()
{
    struct uinput_setup usetup;

    virtual_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);

    if(!virtual_fd)
    {
        printf("Error - Could not open virtual device\n");
        return 0;
    }

    ioctl(virtual_fd, UI_SET_EVBIT, EV_KEY);
    enable_mouse_events(virtual_fd);
    enable_keyboard_events(virtual_fd);

    // some metadata for the input device...
    memset(&usetup, 0, sizeof(usetup));
    usetup.id.bustype = BUS_USB;
    usetup.id.vendor = 0x1234; // sample vendor
    usetup.id.product = 0x5678; // sample product
    strcpy(usetup.name, "DelayDaemon");

    // actually create the device...
    ioctl(virtual_fd, UI_DEV_SETUP, &usetup);
    ioctl(virtual_fd, UI_DEV_CREATE);

    return 1;
}

int init_evdev_input()
{
	/* Open device. */
    // input_fd = open(event_handle, O_RDONLY | O_NONBLOCK);
	fd_event = open(event_handle, O_RDONLY);
	if (fd_event < 0) {
		perror("Failed to open input device");
		exit(EXIT_FAILURE);
	}

	/* Create libevdev device and grab it. */
	if (libevdev_new_from_fd(fd_event, &event_dev) < 0) {
		perror("Failed to init libevdev");
		exit(EXIT_FAILURE);
	}

	if (libevdev_grab(event_dev, LIBEVDEV_GRAB) < 0) {
		perror("Failed to grab device");
		exit(EXIT_FAILURE);
	}
    return 1;
}

int init_evdev_virtual_input()
{
    /* Create uinput clone of device. */
	int fd_uinput = open("/dev/uinput", O_WRONLY);

	if (fd_uinput < 0) {
		perror("Failed to open uinput device");
		exit(EXIT_FAILURE);
	}

	if (libevdev_uinput_create_from_device(event_dev,
				fd_uinput, &uinput_dev) < 0) {
		perror("Failed to create uinput device");
		exit(EXIT_FAILURE);
	}
    return 1;
}

// open the input device we want to "enhance" with delay
int init_input_device()
{
    if(DEBUG) printf("input event: %s\n", event_handle);

    int input_fd = open(event_handle, O_RDONLY | O_NONBLOCK);

    if(DEBUG) printf("input device fd: %d\n", input_fd);

    if(!input_fd)
    {
        printf("Error - Device not found: %d\n", input_fd);
        return 0;
    }

    // this line reserves the device for this program so its events do not arrive at other applications
    ioctl(input_fd, EVIOCGRAB, 1);

    return 1;
}

// make sure to clean up when the program ends
void onExit(int signum)
{
    write_event_log(&ev);
    printf("foo");
    // end inter process communication
    pthread_cancel(fifo_thread);
    unlink(fifo_path);

    // close virtual input device
    ioctl(virtual_fd, UI_DEV_DESTROY);
    close(virtual_fd);


    exit(EXIT_SUCCESS);
}

int main(int argc, char* argv[]) 
{
    signal(SIGINT, onExit);

    // check arguments
    // event_handle is mandatory
    // if only one delay value is passed, the added delay is constant
    if(argc <= 2)
    {
        printf("Too few arguments!\n"
               "Usage: latency_daemon [event_handle] [min_delay_move] [max_delay_move] [fifo_path]\n"
               "event_handle: path to input device you want to delay (e.g. /dev/input/event5)\n"
               "min_delay_click: minimum delay to be added to click events (in milliseconds)\n"
               "max_delay_click: maximum delay to be added to click events (in milliseconds)\n"
               "min_delay_move: minimum delay to be added to mouse movement (in milliseconds)\n"
               "max_delay_move: maximum delay to be added to mouse movement (in milliseconds)\n"
               "distribution: [l]inear (default) or [n]ormal"
               "fifo_path: path to a FIFO used to remotely set delay times during runtime (optional). input \"none\" if unused\n"
               "mu: mean for the normal distribution, if used"
               "sigma: std for the normal distribution, if used"
               "Use the same value for min and max to achieve constant delays.\n");
        return 1;
    }

    event_handle = argv[1];

    init_vector(&ev, 10);

    // prevents Keydown events for KEY_Enter from never being released when grabbing the input device
    // after running the program in a terminal by pressing Enter
    // https://stackoverflow.com/questions/41995349
    sleep(1);

    // if(!init_input_device()) return 1;
    // if(!init_virtual_input()) return 1;

    if(!init_evdev_input()) return 1;
    // if(!init_virtual_input()) return 1;
    if(!init_evdev_virtual_input()) return 1;

    if(sscanf(argv[2], "%d", &min_delay_click) == EOF) min_delay_click = 0;
    if(sscanf(argv[3], "%d", &max_delay_click) == EOF) max_delay_click = min_delay_click;
    if(sscanf(argv[4], "%d", &min_delay_move) == EOF) min_delay_move = 0;
    if(sscanf(argv[5], "%d", &max_delay_move) == EOF) max_delay_move = min_delay_move;

    if(argc > 6)
    {
        char d;
        sscanf(argv[6], "%c", &d);
        switch (d)
        {
        case 'l':
            distribution = linear;
            break;
        case 'n':
            distribution = normal;
            break;
        default:
            distribution = linear;
            break;
        }
    }

    // path to a FIFO to enable inter process communication for remotely controlling the delay times (optional)
    if(argc > 7)
    {
        if(!strcmp(argv[7], "none"))
        {
            fifo_path = argv[7];
            if(!init_fifo()) return 1;
        }
    }

    if(argc > 8)
    {
        sscanf(argv[8], "%lf", &mu);
        sscanf(argv[9], "%lf", &sigma);
    }
    else
    {
        // if mean for normal distribution is not specified, default to the mean of min and max delay
        mu = (max_delay_click + min_delay_click) / 2;
        // if not specified, default to 10% std
        sigma = mu / 20; 
    }

    if(mu>max_delay_click || mu<min_delay_click)
    {
        printf("Illegal value for mu. Average must be between min and max delay!\n");
        return 1;
    }

    if(DEBUG) printf("click delay: %d - %d\nmove delay: %d - %d\n", min_delay_click, max_delay_click, min_delay_move, max_delay_move);

    srand(time(0));

    printf("mu: %lf, sigma: %lf\n", mu, sigma);

    pthread_attr_setdetachstate(&invoked_event_thread_attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setdetachstate(&log_delay_val_thread_attr, PTHREAD_CREATE_DETACHED);

    /* Create RTC interrupts. */
	int fd_rtc = open("/dev/rtc", O_RDONLY);

	if (fd_rtc < 0) {
		perror("Failed to open RTC timer");
		exit(EXIT_FAILURE);
	}

	if (ioctl(fd_rtc, RTC_IRQP_SET, polling_rate) < 0) {
		perror("Failed to set RTC interrupts");
		exit(EXIT_FAILURE);
	}

	if (ioctl(fd_rtc, RTC_PIE_ON, 0) < 0) {
		perror("Failed to enable  RTC interrupts");
		exit(EXIT_FAILURE);
	}

    struct input_event inputEvent;
    int err = -1;
    int rc;
    // wait for new input events of the actual device
    // when new event arrives, generate a delay value and create a thread waiting for this delay time
    // the thread then generates an input event for a virtual input device
    // note EV_SYN events are NOT delayed, they are automatically generated when the delayed event is executed
    while(err = read(fd_rtc, NULL, sizeof(unsigned long)))
    // while(1)
    {
        // err = read(fd_event, &inputEvent, sizeof(struct input_event));
        // err = get_event(&inputEvent);
        err = get_event(&inputEvent);
        if(err > -1 && inputEvent.type != EV_SYN)
        // if(err > -1 && inputEvent.type != EV_SYN && inputEvent.type != EV_MSC) // I have no idea what EV_MSC is but it freezes the application (MSC_SCAN!) when moving fast
		{
            delayed_event *event = malloc(sizeof(delayed_event));
            event->fd = virtual_fd;
            event->type = inputEvent.type;
            event->code = inputEvent.code;
            event->value = inputEvent.value;

            if(inputEvent.type == EV_KEY) event->delay = calculate_delay(min_delay_click, max_delay_click);
            else if(inputEvent.type == EV_REL) event->delay = calculate_delay(min_delay_move, max_delay_move);

            pthread_t delayed_event_thread; 
            pthread_create(&delayed_event_thread, &invoked_event_thread_attr, invoke_delayed_evdev_event, event);

            event->timestamp = inputEvent.time.tv_sec * 1000 + inputEvent.time.tv_usec / 1000;
            append_to_vector(&ev, *event);
        }
    }
    return 0;
}
