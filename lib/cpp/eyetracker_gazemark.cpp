// Connects to the eye tracker and marks gaze point to the screen in real time.

#include <assert.h>
#include <stdio.h>

#include "eyetracker_stream.h"
#include "eyetracker_gazemark.h"

using namespace std;


int main() {
    // Init marker display
    init_marker_disp();


    // Connect to default eye-tracker
    tobii_api_t *api;
    tobii_error_t error = tobii_api_create(&api, NULL, NULL);
    assert(error == TOBII_ERROR_NO_ERROR);

    char url[256] = {0};
    error = tobii_enumerate_local_device_urls(api, single_url_receiver, url);
    assert(error == TOBII_ERROR_NO_ERROR && *url != '\0');

    tobii_device_t *device;
    error = tobii_device_create(api, url, &device);
    assert(error == TOBII_ERROR_NO_ERROR);

    printf("\n*** Eye Tracking Device Detected!\n");
    error = print_device_info(device);
    assert(error == TOBII_ERROR_NO_ERROR);

    error = tobii_gaze_point_subscribe(device, gaze_marker_callback, 0);
    assert(error == TOBII_ERROR_NO_ERROR);

    printf("Marking gaze point...\n");
    int is_running = 1000;
    while (--is_running > 0) {
        error = tobii_wait_for_callbacks(1, &device);
        assert(error == TOBII_ERROR_NO_ERROR || error == TOBII_ERROR_TIMED_OUT);

        error = tobii_device_process_callbacks(device);
        assert(error == TOBII_ERROR_NO_ERROR);
    }

    error = tobii_gaze_point_unsubscribe(device);
    assert(error == TOBII_ERROR_NO_ERROR);

    error = tobii_device_destroy(device);
    assert(error == TOBII_ERROR_NO_ERROR);

    error = tobii_api_destroy(api);
    assert(error == TOBII_ERROR_NO_ERROR);

    close_marker_display();

    return 0;
}