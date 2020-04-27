/////////////////////////////////////////////////////////////////////////////
// A class for annotating gaze status from the eyetracker in real time.
// When the gaze point is valid (i.e. a user is present) a gaze_data obj
// is pushed to a circular buffer and the predicted gaze point is annotated
// on the screen.
// An extern "C" wrapper is also defined for select functions.
//
// Author: Dustin Fast <dustin.fast@hotmail.com>
//
/////////////////////////////////////////////////////////////////////////////

// TODO: buff_to_csv(n)

#include <stdio.h>
#include <chrono>

#include <boost/thread.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/circular_buffer.hpp>
#include <cairo/cairo-xlib.h>
#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include "eyetracker.h"

using namespace std;
using namespace std::chrono;
using time_stamp = time_point<system_clock, microseconds>;


/////////////////////////////////////////////////////////////////////////////
// Defs

#define GAZE_MARKER_WIDTH 5
#define GAZE_MARKER_HEIGHT 20
#define GAZE_MARKER_CDEPTH 32
#define GAZE_MARKER_OPAQUENESS 100
#define GAZE_MARKER_BORDER 0

typedef struct gaze_data {
		int x;
        int y;
        time_stamp unixtime_us;
	} gaze_data_t;

void do_gaze_point_subscribe(tobii_device_t*, void*);
void cb_gaze_point(tobii_gaze_point_t const* , void*);


/////////////////////////////////////////////////////////////////////////////
// Class

class EyeTrackerGaze : EyeTracker {
    private:
        boost::thread *m_async;
        boost::mutex *m_async_mutex;

    public:
        Display *m_disp;
        Window m_root_wind;
        XVisualInfo m_vinfo;
        XSetWindowAttributes m_attrs;
        Window m_overlay;
        int m_mark_count;
        int m_mark_freq;
        int m_disp_width;
        int m_disp_height;
        bool m_gaze_is_valid;

        void start();
        void stop();
        bool is_gaze_valid();
        void enque_gaze_data(int, int, time_stamp);
        void print_gaze_data();
    
        EyeTrackerGaze(int, int, int, int);
        ~EyeTrackerGaze();

    protected:
        boost::circular_buffer<gaze_data_t> m_gaze_buff;
};

// Default constructor
EyeTrackerGaze::EyeTrackerGaze(
    int disp_width, int disp_height, int mark_freq, int buff_sz) {
    m_disp_width = disp_width;
    m_disp_height = disp_height;
    m_mark_freq = mark_freq;
    m_gaze_buff = boost::circular_buffer<gaze_data_t>(buff_sz); 

    m_disp = XOpenDisplay(NULL);
    m_root_wind = DefaultRootWindow(m_disp);

    XMatchVisualInfo(
        m_disp, DefaultScreen(m_disp), GAZE_MARKER_CDEPTH, TrueColor, &m_vinfo);

    m_attrs.override_redirect = true;
    m_attrs.colormap = XCreateColormap(
        m_disp, m_root_wind, m_vinfo.visual, AllocNone);
    m_attrs.background_pixel = GAZE_MARKER_OPAQUENESS;
    m_attrs.border_pixel = 0;

    m_mark_count = 0;
    m_gaze_is_valid = False;
}

// Destructor
EyeTrackerGaze::~EyeTrackerGaze() {
    XCloseDisplay(m_disp);
}

// Starts the async gaze data watcher
void EyeTrackerGaze::start() {
    m_async_mutex = new boost::mutex;
    m_async = new boost::thread(do_gaze_point_subscribe, m_device, this);
}

// Stops the async watcher
void EyeTrackerGaze::stop() {
    try {
        m_async->interrupt();
        m_async->join();
        delete m_async;
        delete m_async_mutex;
    }
    catch (boost::exception&) {}
}

// Returns the current gaze validity state
bool EyeTrackerGaze::is_gaze_valid() {
    return m_gaze_is_valid;
}

// Enques gaze data into the circular buffer
void EyeTrackerGaze::enque_gaze_data(int x, int y, time_stamp unixtime_us) {
    // TODO: ensure no mem leak
    gaze_data gd;
    gd.x = x;
    gd.y = y;
    gd.unixtime_us = unixtime_us;

    m_async_mutex->lock();
    m_gaze_buff.push_back(gd);
    m_async_mutex->unlock();
}

// Prints the contents of the circular buffer. For debug convenience.
void EyeTrackerGaze::print_gaze_data() {
    boost::circular_buffer<gaze_data_t>::iterator i; 

    for (i = m_gaze_buff.begin(); i < m_gaze_buff.end(); i++)  {
        printf("(%d, %d)\n", ((gaze_data)*i).x, ((gaze_data)*i).y); 
    }
}


/////////////////////////////////////////////////////////////////////////////
// Extern class wrapper, exposing instantiation and gaze stream start/stop

extern "C"
{
    EyeTrackerGaze* eyetracker_gaze_new(
        int disp_width, int disp_height, int mark_freq, int buff_sz) {
            return new EyeTrackerGaze(
                disp_width, disp_height, mark_freq, buff_sz);
    }

    void eyetracker_gaze_start(EyeTrackerGaze* gaze) {gaze->start();}
    void eyetracker_gaze_stop(EyeTrackerGaze* gaze) {return gaze->stop();}
}


/////////////////////////////////////////////////////////////////////////////
// Gaze subscriber and callback functions

// Starts the gaze data stream
void do_gaze_point_subscribe(tobii_device_t *device, void *gaze) {

    // Subscribe to gaze point
    assert(tobii_gaze_point_subscribe(device, cb_gaze_point, gaze
    ) == NO_ERROR);

    try {
        while (True) {
            assert(tobii_wait_for_callbacks(1, &device) == NO_ERROR);
            assert(tobii_device_process_callbacks(device) == NO_ERROR);
            boost::this_thread::sleep_for(boost::chrono::milliseconds{1});
        }
    }
    catch (boost::thread_interrupted&) {}

    assert(tobii_gaze_point_unsubscribe(device) == NO_ERROR);
}

// Gaze point callback for use with tobii_gaze_point_subscribe(). Gets the
// eyetrackers predicted on-screen gaze coordinates (x, y) and enques gaze
// data into EyeTrackerGazes' circular buffer. Also creates a shaded window
// overlay 
// ASSUMES: user_data is a ptr to an object of type EyeTrackerGaze.
void cb_gaze_point(tobii_gaze_point_t const *gaze_point, void *user_data) {
    EyeTrackerGaze *gaze = static_cast<EyeTrackerGaze*>(user_data);

    // If gaze is detected, do the enque and screen annotation
    if (gaze_point->validity == TOBII_VALIDITY_VALID) {
        gaze->m_gaze_is_valid = True;
        
        // Get unix timestamp in microseconds
        // TODO: Ensure this time is not delayed from gaze_point->timestamp_us
        time_stamp unixtime_us = time_point_cast<microseconds>(
            system_clock::now()
        );

        // Convert gaze point to screen coords
        int x_coord = gaze_point->position_xy[0] * gaze->m_disp_width;
        int y_coord = gaze_point->position_xy[1] * gaze->m_disp_height;
        
        // printf("Gaze points: %d, %d\n", x, y);  // debug
        // printf("Gaze point time: %li\n", gaze_point->timestamp_us); // debug
        // printf("Epoch time: %li\n\n", unixtime_us);  // debug

        // Enque the gaze data in the circle buffer
        gaze->enque_gaze_data(x_coord, y_coord, unixtime_us);

        // Annotate (x, y) on the screen every m_mark_freq callbacks
        gaze->m_mark_count++;
        if (gaze->m_mark_count % gaze->m_mark_freq != 0)
            return;

        gaze->m_mark_count = 0;

        // Create the gaze marker as an overlay window
        gaze->m_overlay = XCreateWindow(
            gaze->m_disp,
            gaze->m_root_wind,
            x_coord,
            y_coord, 
            GAZE_MARKER_WIDTH, 
            GAZE_MARKER_HEIGHT,
            GAZE_MARKER_BORDER,
            gaze->m_vinfo.depth,
            InputOutput, 
            gaze->m_vinfo.visual,
            CWOverrideRedirect | CWColormap | CWBackPixel | CWBorderPixel, 
            &gaze->m_attrs
        );

        XMapWindow(gaze->m_disp, gaze->m_overlay);

        cairo_surface_t* surf = cairo_xlib_surface_create(
            gaze->m_disp, 
            gaze->m_overlay,
            gaze->m_vinfo.visual,
            GAZE_MARKER_WIDTH,
            GAZE_MARKER_HEIGHT);

        // Destroy the marker
        XFlush(gaze->m_disp);
        cairo_surface_destroy(surf);
        XUnmapWindow(gaze->m_disp, gaze->m_overlay);
    }

    // Else if no gaze detected, do nothing
    else {
        gaze->m_gaze_is_valid = False;
        // printf("WARN: Received invalid gaze_point.\n"); // debug

    }
}