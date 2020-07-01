/////////////////////////////////////////////////////////////////////////////
// A class for annotating gaze status from the eyetracker in real time.
// When the gaze point is valid (i.e. a user is present) gaze_data
// objects are pushed to a circular buffer and the predicted gaze point is
// annotated on the screen. Buffer contents may also be written to CSV.
// Extern "C" wrappers are defined for select functions.
//
// Author: Dustin Fast <dustin.fast@hotmail.com>
//
/////////////////////////////////////////////////////////////////////////////

#include <fstream>

#include <boost/thread.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/circular_buffer.hpp>

#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include "eyetracker.h"

using namespace std;

/////////////////////////////////////////////////////////////////////////////
// Defs

#define GAZE_MARKER_WIDTH 3
#define GAZE_MARKER_HEIGHT 20
#define GAZE_MARKER_BORDER 0
#define GAZE_MARKER_BORDER 0
#define MOUNT_OFFSET_MM 0.0

typedef struct gaze_data {
        int64_t unixtime_us;

        float left_pupildiameter_mm;
        float right_pupildiameter_mm;

        float left_eyeposition_normed_x;
		float left_eyeposition_normed_y;
		float left_eyeposition_normed_z;
		float right_eyeposition_normed_x;
		float right_eyeposition_normed_y;
		float right_eyeposition_normed_z;

        float left_eyecenter_mm_x;
		float left_eyecenter_mm_y;
		float left_eyecenter_mm_z;
		float right_eyecenter_mm_x;
		float right_eyecenter_mm_y;
		float right_eyecenter_mm_z;

        float left_gazeorigin_mm_x;
		float left_gazeorigin_mm_y;
		float left_gazeorigin_mm_z;
		float right_gazeorigin_mm_x;
		float right_gazeorigin_mm_y;
		float right_gazeorigin_mm_z;

        float left_gazepoint_mm_x;
		float left_gazepoint_mm_y;
		float left_gazepoint_mm_z;
		float right_gazepoint_mm_x;
		float right_gazepoint_mm_y;
		float right_gazepoint_mm_z;

        float left_gazepoint_normed_x;
		float left_gazepoint_normed_y;
		float right_gazepoint_normed_x;
		float right_gazepoint_normed_y;

        int combined_gazepoint_x;
        int combined_gazepoint_y;
	    } gaze_data_t;

typedef struct gaze_point {
        int n_samples;
        int x_coord;
        int y_coord;
	    } gaze_point_t;

typedef boost::circular_buffer<shared_ptr<gaze_data_t>> circ_buff;

void do_gaze_data_subscribe(tobii_device_t*, void*);
static void cb_gaze_data(tobii_gaze_data_t const*, void*);
XColor createXColorFromRGBA(void*, short, short, short, short);


/////////////////////////////////////////////////////////////////////////////
// Class

class EyeTrackerGaze : public EyeTracker {
    public:
        int m_mark_count;
        int m_mark_freq;
        int m_smooth_over;
        Display *m_disp;
        Window m_overlay;

        void start();
        void stop();
        int gaze_data_tocsv(const char*, int, boost::shared_ptr<char>);
        bool is_gaze_valid();
        void enque_gaze_data(shared_ptr<gaze_data_t>);
        void set_gaze_marker(shared_ptr<gaze_data_t>);
        void print_gaze_data();
        int gaze_data_sz();
        int disp_x_from_normed_x(float);
        int disp_y_from_normed_y(float);
        gaze_point_t* get_gazepoint();

        EyeTrackerGaze(float, float, int, int, int, int, int);
        ~EyeTrackerGaze();

    protected:
        int m_disp_width;
        int m_disp_height;
        int m_buff_sz;
        shared_ptr<circ_buff> m_gaze_buff;

    private:
        shared_ptr<boost::thread> m_async_streamer;
        shared_ptr<boost::thread> m_async_writer;
        shared_ptr<boost::mutex> m_async_mutex;
};

// Default constructor
// TODO: Param docstring
EyeTrackerGaze::EyeTrackerGaze(float disp_width_mm, 
                               float disp_height_mm,
                               int disp_width_px,
                               int disp_height_px,
                               int mark_freq,
                               int buff_sz,
                               int smooth_over) {
        // Init members from args
        m_disp_width = disp_width_px;
        m_disp_height = disp_height_px;
        m_mark_freq = mark_freq;
        m_buff_sz = buff_sz;
        m_smooth_over = smooth_over;

        // Calibrate gaze tracker's disp area
        set_display(disp_width_mm, disp_height_mm, MOUNT_OFFSET_MM);

        // Since we care about device timestamps, start time synchronization
        sync_device_time();

        // Init circular gaze data buffer and mutex 
        m_gaze_buff = make_shared<circ_buff>(buff_sz); 
        m_async_mutex = make_shared<boost::mutex>();

        // Set default tracker states
        m_mark_count = 0;
        m_async_writer = NULL;
        m_async_streamer = NULL;

        // Init X11 display
        m_disp = XOpenDisplay(NULL);
        Window root_win = DefaultRootWindow(m_disp);

        XVisualInfo vinfo;
        XMatchVisualInfo(
            m_disp,
            DefaultScreen(m_disp),
            32,
            TrueColor,
            &vinfo
        );

        // Create the gaze marker (as an X11 window)
        XSetWindowAttributes attrs;
        attrs.save_under= true;
        attrs.override_redirect = true;
        attrs.border_pixel = 0;
        attrs.background_pixel = createXColorFromRGBA(
            this, 255, 100, 0, 175).pixel;
        attrs.colormap = XCreateColormap(
            m_disp, root_win, vinfo.visual, AllocNone);

        m_overlay = XCreateWindow(
            m_disp,
            root_win,
            0, 0, 
            GAZE_MARKER_WIDTH, 
            GAZE_MARKER_HEIGHT,
            GAZE_MARKER_BORDER,
            vinfo.depth,
            InputOutput, 
            vinfo.visual,
            CWSaveUnder | 
            CWOverrideRedirect | 
            CWBackPixel | 
            CWBorderPixel | 
            CWColormap, 
            &attrs
        );

        XMapWindow(m_disp, m_overlay);
}

// Destructor
EyeTrackerGaze::~EyeTrackerGaze() {
    XUnmapWindow(m_disp, m_overlay);
    XFlush(m_disp);
    XCloseDisplay(m_disp);
}

// Starts the async gaze threads
void EyeTrackerGaze::start() {
    if (m_async_streamer) {
        printf("ERROR: Gaze stream already running.");
    } else {
        m_async_streamer = make_shared<boost::thread>(
            do_gaze_data_subscribe, m_device, this
        );
    }
}

// Stops the async gaze threads
void EyeTrackerGaze::stop() {
    // Stop the gaze data streamer with an interrupt
    if (m_async_streamer) {
        m_async_streamer->interrupt();
        m_async_streamer->join();
        m_async_streamer = NULL;
    }

    // Wait for writer thread to finish its current write
    if (m_async_writer) {
        m_async_writer->join();
        m_async_writer = NULL;
    }
}

// Writes the gaze data to the given csv file path, creating it if exists 
// else appending to it. If n is given, writes only the most recent n samples.
// Returns an int representing the number of samples written. If label given,
// appends the given cstring to each csv row written.
int EyeTrackerGaze::gaze_data_tocsv(
    const char *file_path, int n=0, boost::shared_ptr<char> label=NULL) {
    // Copy circ buff contents then (effectively) clear it
    m_async_mutex->lock();
    shared_ptr<circ_buff> gaze_buff = m_gaze_buff;
    m_gaze_buff = make_shared<circ_buff>(m_buff_sz);
    m_async_mutex->unlock();

    // Get buff content count and return if empty
    int sample_count = gaze_buff->size();
    if (sample_count <= 0)
        return 0;
    
    // n == 0 denotes write entire buff contents
    if (n == 0)
        n = sample_count;

    // Ensure any previous async write job has finished
    if (m_async_writer) {
        m_async_writer->join();
    }

    // Write the gaze data to file asynchronously
    m_async_writer = make_shared<boost::thread>(
        [file_path, gaze_buff, n, label]() {
            ofstream f, f2;
            f.open(file_path, fstream::in | fstream::out | fstream::app);

            // Write (at most) the n latest samples to csv in ascending order
            int sz = gaze_buff->size();
            int n_capped = min(sz, n);

            for (int j = sz - n_capped; j < sz; j++)  {
                auto cgd = *gaze_buff->at(j); 
                f << 
                    cgd.unixtime_us << ", " <<
                    cgd.left_pupildiameter_mm << ", " <<
                    cgd.right_pupildiameter_mm << ", " <<
                    cgd.left_eyeposition_normed_x << ", " <<
                    cgd.left_eyeposition_normed_y << ", " <<
                    cgd.left_eyeposition_normed_z << ", " <<
                    cgd.right_eyeposition_normed_x << ", " <<
                    cgd.right_eyeposition_normed_y << ", " <<
                    cgd.right_eyeposition_normed_z << ", " <<
                    cgd.left_eyecenter_mm_x << ", " <<
                    cgd.left_eyecenter_mm_y << ", " <<
                    cgd.left_eyecenter_mm_z << ", " <<
                    cgd.right_eyecenter_mm_x << ", " <<
                    cgd.right_eyecenter_mm_y << ", " <<
                    cgd.right_eyecenter_mm_z << ", " <<
                    cgd.left_gazeorigin_mm_x << ", " <<
                    cgd.left_gazeorigin_mm_y << ", " <<
                    cgd.left_gazeorigin_mm_z << ", " <<
                    cgd.right_gazeorigin_mm_x << ", " <<
                    cgd.right_gazeorigin_mm_y << ", " <<
                    cgd.right_gazeorigin_mm_z << ", " <<
                    cgd.left_gazepoint_mm_x << ", " <<
                    cgd.left_gazepoint_mm_y << ", " <<
                    cgd.left_gazepoint_mm_z << ", " <<
                    cgd.right_gazepoint_mm_x << ", " <<
                    cgd.right_gazepoint_mm_y << ", " <<
                    cgd.right_gazepoint_mm_z << ", " <<
                    cgd.left_gazepoint_normed_x << ", " <<
                    cgd.left_gazepoint_normed_y << ", " <<
                    cgd.right_gazepoint_normed_x << ", " <<
                    cgd.right_gazepoint_normed_y << ", " <<
                    cgd.combined_gazepoint_x << ", " <<
                    cgd.combined_gazepoint_y;
                    
                if (label != NULL)
                    f << ", " << label;
                
                f << "\n";
            }

            f.close();
        }
    );

    return sample_count;
}

// Enques gaze data into the circular buffer.
void EyeTrackerGaze::enque_gaze_data(shared_ptr<gaze_data_t> cgd) {
    m_async_mutex->lock();
    m_gaze_buff->push_back(cgd);
    m_async_mutex->unlock();
}

// Prints the coord contents of the circular buffer. For debug convenience.
void EyeTrackerGaze::print_gaze_data() {
    circ_buff::iterator i; 

    m_async_mutex->lock();
    for (i = m_gaze_buff->begin(); i < m_gaze_buff->end(); i++)  {
        printf("(%d, %d)\n",
        i->get()->combined_gazepoint_x,
        i->get()->combined_gazepoint_y); 
    }
    m_async_mutex->unlock();

    printf("Gaze sample count: %li\n", m_gaze_buff->size());
}

// Returns the current number of gaze points in the gaze data buffer.
// Note: The caller of this func is responsible for calling mutex lock/unlock.
int EyeTrackerGaze::gaze_data_sz() {
    return m_gaze_buff->size();
}

// Given a normalized gaze point's x coord, returns the x in display coords.
int EyeTrackerGaze::disp_x_from_normed_x(float x_normed) {
    return x_normed * m_disp_width;
}

// Given a normalized gaze point's y coord, returns the x in display coords.
int EyeTrackerGaze::disp_y_from_normed_y(float y_normed) {
    return y_normed * m_disp_height;
}

// Returns the current smoothed display gazepoint.
gaze_point_t* EyeTrackerGaze::get_gazepoint() {
    int avg_x = 0;
    int avg_y = 0;
    int buff_sz = 0;
    int n_samples = 0;

    // Average the gaze pt from (at most) the m_smooth_over latest samples
    m_async_mutex->lock();
    buff_sz = gaze_data_sz();
    n_samples = min(buff_sz, m_smooth_over);
    
    for (int j = buff_sz - n_samples; j < buff_sz; j++)  {
        auto cgd = *m_gaze_buff->at(j); 
        avg_x += cgd.combined_gazepoint_x;
        avg_y += cgd.combined_gazepoint_y;
    }

    m_async_mutex->unlock();

    if (n_samples > 0) {
        avg_x = avg_x / n_samples;
        avg_y = avg_y / n_samples; 
    }

    gaze_point_t *gp = new(gaze_point_t);
    gp->n_samples = n_samples;
    gp->x_coord = avg_x;
    gp->y_coord = avg_y;

    return gp;
}

// Sets or updates the on-screen gaze marker position.
void EyeTrackerGaze::set_gaze_marker(shared_ptr<gaze_data_t> cgd) {
    // TODO: Iff infer == True, get predicted coords
    
    // Else...
    XMoveWindow(
        m_disp,
        m_overlay, 
        cgd->combined_gazepoint_x,
        cgd->combined_gazepoint_y
    );

    XFlush(m_disp);
}

/////////////////////////////////////////////////////////////////////////////
// Extern wrapper exposing a subset of EyeTrackerGaze()'s methods
extern "C" {
    EyeTrackerGaze* eye_gaze_new(
        float disp_width_mm, float disp_height_mm, int disp_width_px, 
            int disp_height_px, int mark_freq, int buff_sz, int smooth_over) {
                return new EyeTrackerGaze(
                    disp_width_mm, disp_height_mm, disp_width_px,
                        disp_height_px, mark_freq, buff_sz, smooth_over
                );
    }

    void eye_gaze_destructor(EyeTrackerGaze* gaze) {
        gaze->~EyeTrackerGaze();
    }

    int eye_gaze_data_tocsv(
        EyeTrackerGaze* gaze, const char *file_path, int n, const char *label) {
            boost::shared_ptr<char> p_label(new char[strlen(label)+1]);
            strcpy(p_label.get(), label);

            return gaze->gaze_data_tocsv(file_path, n, p_label);
    }

    void eye_gaze_start(EyeTrackerGaze* gaze) {
        gaze->start();
    }

    void eye_gaze_stop(EyeTrackerGaze* gaze) {
        gaze->stop();
    }

    int eye_gaze_data_sz(EyeTrackerGaze* gaze) {
        return gaze->gaze_data_sz();
    }

    gaze_point_t* eye_gaze_point(EyeTrackerGaze* gaze) {
        return gaze->get_gazepoint();
    }

    void eye_gaze_point_free(gaze_point_t *gp) {
        delete gp;
    }
}


/////////////////////////////////////////////////////////////////////////////
// Gaze subscriber and callback functions

// Starts the gaze point data stream
void do_gaze_data_subscribe(tobii_device_t *device, void *gaze) {

    // Subscribe to gaze point
    assert(tobii_gaze_data_subscribe(device, cb_gaze_data, gaze
    ) == NO_ERROR);

    try {
        while (True) {
            assert(tobii_wait_for_callbacks(1, &device) == NO_ERROR);
            assert(tobii_device_process_callbacks(device) == NO_ERROR);
            boost::this_thread::sleep_for(boost::chrono::microseconds{1});
        }
    } catch (boost::thread_interrupted&) {}

    assert(tobii_gaze_data_unsubscribe(device) == NO_ERROR);
}


// Gaze point callback for use with tobii_gaze_point_subscribe(). Gets the
// eyetrackers predicted on-screen gaze coordinates (x, y) and enques gaze
// data into EyeTrackerGazes' circular buffer. Also creates a shaded window
// overlay denoting the gaze point on the screen.
// ASSUMES: user_data is a ptr to an object of type EyeTrackerGaze.
static void cb_gaze_data(tobii_gaze_data_t const *data, void *user_data) {
    auto *gaze = static_cast<EyeTrackerGaze*>(user_data);

    if(data->left.gaze_point_validity == TOBII_VALIDITY_VALID ==
        data->right.gaze_point_validity) {
        
        // Convert gaze point to screen coords
        int left_gazepoint_x = gaze->disp_x_from_normed_x(
            data->left.gaze_point_on_display_normalized_xy[0]);
        int left_gazepoint_y = gaze->disp_y_from_normed_y(
            data->left.gaze_point_on_display_normalized_xy[1]);
            
        int right_gazepoint_x = gaze->disp_x_from_normed_x(
            data->right.gaze_point_on_display_normalized_xy[0]);
        int right_gazepoint_y = gaze->disp_y_from_normed_y(
            data->right.gaze_point_on_display_normalized_xy[1]);

        int x_gazepoint = (left_gazepoint_x + right_gazepoint_x) / 2;
        int y_gazepoint = (left_gazepoint_y + right_gazepoint_y) / 2;

        // Convert timestamp from device time to system clock time
        int64_t timestamp_us = gaze->devicetime_to_systime(
            data->timestamp_system_us);

        // Copy gaze data then enque it in the EyeTrackerGaze buff
        shared_ptr<gaze_data_t> cgd = make_shared<gaze_data_t>();

        cgd->unixtime_us = timestamp_us;
        cgd->left_pupildiameter_mm = data->left.pupil_diameter_mm;
        cgd->right_pupildiameter_mm = data->right.pupil_diameter_mm;
        cgd->left_eyeposition_normed_x = 
            data->left.eye_position_in_track_box_normalized_xyz[0];
		cgd->left_eyeposition_normed_y = 
            data->left.eye_position_in_track_box_normalized_xyz[1];
		cgd->left_eyeposition_normed_z = 
            data->left.eye_position_in_track_box_normalized_xyz[2];
		cgd->right_eyeposition_normed_x = 
            data->right.eye_position_in_track_box_normalized_xyz[0];
		cgd->right_eyeposition_normed_y = 
            data->right.eye_position_in_track_box_normalized_xyz[1];
		cgd->right_eyeposition_normed_z = 
            data->right.eye_position_in_track_box_normalized_xyz[2];
        cgd->left_eyecenter_mm_x = 
            data->left.eyeball_center_from_eye_tracker_mm_xyz[0];
		cgd->left_eyecenter_mm_y = 
            data->left.eyeball_center_from_eye_tracker_mm_xyz[1];
		cgd->left_eyecenter_mm_z = 
            data->left.eyeball_center_from_eye_tracker_mm_xyz[2];
		cgd->right_eyecenter_mm_x = 
            data->right.eyeball_center_from_eye_tracker_mm_xyz[0];
		cgd->right_eyecenter_mm_y = 
            data->right.eyeball_center_from_eye_tracker_mm_xyz[1];
		cgd->right_eyecenter_mm_z = 
            data->right.eyeball_center_from_eye_tracker_mm_xyz[2];
        cgd->left_gazeorigin_mm_x = 
            data->left.gaze_origin_from_eye_tracker_mm_xyz[0];
		cgd->left_gazeorigin_mm_y = 
            data->left.gaze_origin_from_eye_tracker_mm_xyz[1];
		cgd->left_gazeorigin_mm_z = 
            data->left.gaze_origin_from_eye_tracker_mm_xyz[2];
		cgd->right_gazeorigin_mm_x = 
            data->right.gaze_origin_from_eye_tracker_mm_xyz[0];
		cgd->right_gazeorigin_mm_y = 
            data->right.gaze_origin_from_eye_tracker_mm_xyz[1];
		cgd->right_gazeorigin_mm_z = 
            data->right.gaze_origin_from_eye_tracker_mm_xyz[2];
        cgd->left_gazepoint_mm_x = 
            data->left.gaze_point_from_eye_tracker_mm_xyz[0];
		cgd->left_gazepoint_mm_y = 
            data->left.gaze_point_from_eye_tracker_mm_xyz[1];
		cgd->left_gazepoint_mm_z = 
            data->left.gaze_point_from_eye_tracker_mm_xyz[2];
		cgd->right_gazepoint_mm_x = 
            data->right.gaze_point_from_eye_tracker_mm_xyz[0];
		cgd->right_gazepoint_mm_y = 
            data->right.gaze_point_from_eye_tracker_mm_xyz[1];
		cgd->right_gazepoint_mm_z = 
            data->right.gaze_point_from_eye_tracker_mm_xyz[2];
        cgd->left_gazepoint_normed_x = 
            data->left.gaze_point_on_display_normalized_xy[0];
		cgd->left_gazepoint_normed_y = 
            data->left.gaze_point_on_display_normalized_xy[1];
		cgd->right_gazepoint_normed_x = 
            data->right.gaze_point_on_display_normalized_xy[0];
		cgd->right_gazepoint_normed_y = 
            data->right.gaze_point_on_display_normalized_xy[1];
        cgd->combined_gazepoint_x = x_gazepoint;
        cgd->combined_gazepoint_y = y_gazepoint;

        gaze->enque_gaze_data(cgd);

        // Annotate (x, y) on the screen every m_mark_freq callbacks
        gaze->m_mark_count++;
        if (gaze->m_mark_count % gaze->m_mark_freq != 0)
            return;

        // Else, reset the gaze mark count & update marker position
        gaze->m_mark_count = 0;
        gaze->set_gaze_marker(cgd);
    }
    else {
        // printf("WARN: Gaze point invalid.");  // Debug
    }
}

// Helper for creating an XColor for the gaze display
// Adapted from gist.github.com/ericek111/774a1661be69387de846f5f5a5977a46
XColor createXColorFromRGBA(void *gz, short r, short g, short b, short alpha) {
    EyeTrackerGaze *gaze = static_cast<EyeTrackerGaze*>(gz);
    XColor color;

    // m_color.red = red * 65535 / 255;
    color.red = (r * 0xFFFF) / 0xFF;
    color.green = (g * 0xFFFF) / 0xFF;
    color.blue = (b * 0xFFFF) / 0xFF;
    color.flags = DoRed | DoGreen | DoBlue;

    XAllocColor(
        gaze->m_disp,
        DefaultColormap(gaze->m_disp, DefaultScreen(gaze->m_disp)),
        &color
    );

    *(&color.pixel) = ((*(&color.pixel)) & 0x00ffffff) | (alpha << 24);

    return color;
}