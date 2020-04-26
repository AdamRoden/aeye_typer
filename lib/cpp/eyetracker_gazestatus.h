// A class for annotating gaze status from the eyetracker in real time.
// Gaze point is displayed on the screen when valid.

#include <stdio.h>
#include <cstring>

#include <X11/Xlib.h>
#include <X11/X.h>
#include <X11/Xutil.h>

#include <tobii/tobii.h>
#include <tobii/tobii_streams.h>

#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>

#include <boost/circular_buffer.hpp>

#include "eyetracker.h"

using namespace std;


/////////////////////////////////////////////////////////////////////////////
// Definitions

#define GAZE_MARKER_WIDTH 5
#define GAZE_MARKER_HEIGHT 20
#define GAZE_MARKER_CDEPTH 32
#define GAZE_MARKER_OPAQUENESS 100
#define GAZE_MARKER_BORDER 0

void cb_gaze_point(tobii_gaze_point_t const *gaze_point, void *user_data);

typedef struct gaze_data {
		int x;
        int y;
        int64_t timestamp_us;
	} gaze_data_t;


/////////////////////////////////////////////////////////////////////////////
// Class Def
class EyeTrackerGaze : EyeTracker {
    boost::circular_buffer<gaze_data_t> gaze_buff;
    int default_screen;

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

        EyeTrackerGaze(int, int, int, int);
        ~EyeTrackerGaze();
        void start(int);
        void start_async(int);
        bool is_gaze_valid();
        void enque_gaze_data(int x, int y, int64_t);
        void print_gaze_data();
};

// Default constructor
EyeTrackerGaze::EyeTrackerGaze(
    int disp_width, int disp_height, int update_freq, int buff_sz) {
    m_disp_width = disp_width;
    m_disp_height = disp_height;
    m_mark_freq = update_freq;
    gaze_buff = boost::circular_buffer<gaze_data_t>(buff_sz); 

    m_disp = XOpenDisplay(NULL);
    m_root_wind = DefaultRootWindow(m_disp);
    default_screen = XDefaultScreen(m_disp);

    XMatchVisualInfo(
        m_disp, DefaultScreen(m_disp), GAZE_MARKER_CDEPTH, TrueColor, &m_vinfo);

    m_attrs.override_redirect = true;
    m_attrs.colormap = XCreateColormap(m_disp, m_root_wind, m_vinfo.visual, AllocNone);
    m_attrs.background_pixel = GAZE_MARKER_OPAQUENESS;
    m_attrs.border_pixel = 0;

    m_mark_count = 0;
    m_gaze_is_valid = False;
}

// Destructor
EyeTrackerGaze::~EyeTrackerGaze() {
        XCloseDisplay(m_disp);
}

// Starts watching the gaze data stream for the given number of iterations.
// If iterations == 0, runs indefinately.
void EyeTrackerGaze::start(int iterations=100) {
    // TODO: Subscribe to user position, etc.

    // Subscribe to gaze point
    assert(tobii_gaze_point_subscribe(m_device, cb_gaze_point, this
    ) == TOBII_ERROR_NO_ERROR);

    bool run_indef = False;
    if (iterations == 0)
        run_indef = True;

    while (run_indef || --iterations > 0) {
        assert(tobii_wait_for_callbacks(1, &m_device) == TOBII_ERROR_NO_ERROR);
        assert(tobii_device_process_callbacks(m_device) == TOBII_ERROR_NO_ERROR);
    }

    assert(tobii_gaze_point_unsubscribe(m_device) == TOBII_ERROR_NO_ERROR);

}

// Starts watching the gaze data stream asynchronously for the given number of
// iterations. If iterations == 0, runs indefinately.
// TODO: void EyeTrackerGaze::start_async(int iterations=100) {
// }

// Returns the current gaze validity state
bool EyeTrackerGaze::is_gaze_valid() {
    return m_gaze_is_valid;
}

// Enques gaze data into the circular buffer
void EyeTrackerGaze::enque_gaze_data(int x, int y, int64_t timestamp) {
    // TODO: mutex
    gaze_data gd;
    gd.x = x;
    gd.y = y;
    gd.timestamp_us = timestamp;

    gaze_buff.push_back(gd);
}

// Prints the contents of the circular buffer. For debug convenience.
void EyeTrackerGaze::print_gaze_data() {
    boost::circular_buffer<gaze_data_t>::iterator i; 

    for (i = gaze_buff.begin(); i < gaze_buff.end(); i++)  {
        printf("%li\n", ((gaze_data)*i).timestamp_us); 
    }
}


/////////////////////////////////////////////////////////////////////////////
// Gaze point callback, for use with tobii_gaze_point_subscribe(). Gets the
// eyetrackers predicted on-screen gaze coordinates (x, y) and enques gaze
// data into Gazes' circular buffer.
// ASSUMES: user_data is a ptr to an object of type EyeTrackerGaze.
void cb_gaze_point(tobii_gaze_point_t const *gaze_point, void *user_data) {
    EyeTrackerGaze *gaze_status = static_cast<EyeTrackerGaze*>(user_data);

    // Only process every m_mark_freq callbacks
    gaze_status->m_mark_count++;
    if (gaze_status->m_mark_count % gaze_status->m_mark_freq != 0)
        return;

    gaze_status->m_mark_count = 0;

    if (gaze_point->validity == TOBII_VALIDITY_VALID) {
        gaze_status->m_gaze_is_valid = True;

        // Convert gaze point to screen coords
        int x_coord = gaze_point->position_xy[0] * gaze_status->m_disp_width;
        int y_coord = gaze_point->position_xy[1] * gaze_status->m_disp_height;

        // Enque the gaze data in the circle buffer
        gaze_status->enque_gaze_data(x_coord, y_coord, gaze_point->timestamp_us);

        // printf("Gaze points: %d, %d\n", x, y);  // debug
        printf("Gaze time: %li\n", gaze_point->timestamp_us);  // debug

        // Create the gaze marker as an overlay window
        gaze_status->m_overlay = XCreateWindow(
            gaze_status->m_disp,
            gaze_status->m_root_wind,
            x_coord,
            y_coord, 
            GAZE_MARKER_WIDTH, 
            GAZE_MARKER_HEIGHT,
            GAZE_MARKER_BORDER,
            gaze_status->m_vinfo.depth,
            InputOutput, 
            gaze_status->m_vinfo.visual,
            CWOverrideRedirect | CWColormap | CWBackPixel | CWBorderPixel, 
            &gaze_status->m_attrs
        );

        XMapWindow(gaze_status->m_disp, gaze_status->m_overlay);

        cairo_surface_t* surf = cairo_xlib_surface_create(
            gaze_status->m_disp, 
            gaze_status->m_overlay,
            gaze_status->m_vinfo.visual,
            GAZE_MARKER_WIDTH,
            GAZE_MARKER_HEIGHT);

        // Destroy the marker
        XFlush(gaze_status->m_disp);
        cairo_surface_destroy(surf);
        XUnmapWindow(gaze_status->m_disp, gaze_status->m_overlay);
    }
    else
    {
        gaze_status->m_gaze_is_valid = False;
        printf("WARN: Received invalid gaze_point.\n"); // debug

    }
}
