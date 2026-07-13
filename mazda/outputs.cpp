#include "outputs.h"
#include "main.h"
#include "callbacks.h"

#include "json/json.hpp"
using json = nlohmann::json;

#include <linux/input.h>
#include <linux/uinput.h>

#define EVENT_DEVICE_TS	"/dev/input/filtered-touchscreen0"
#define EVENT_DEVICE_KBD "/dev/input/filtered-keyboard0"
#define EVENT_DEVICE_UI "/dev/uinput"

static gboolean bus_callback(GstBus *bus, GstMessage *message, gpointer *ptr)
{
    gst_app_t *app = &gst_app;

    switch(GST_MESSAGE_TYPE(message)){

        case GST_MESSAGE_ERROR:{
                           gchar *debug;
                           GError *err;

                           gst_message_parse_error(message, &err, &debug);
                           g_print("Error %s\n", err->message);
                           g_error_free(err);
                           g_free(debug);
                           g_main_loop_quit(app->loop);
                       }
                       break;

        case GST_MESSAGE_WARNING:{
                         gchar *debug;
                         GError *err;
                         gchar *name;

                         gst_message_parse_warning(message, &err, &debug);
                         g_print("Warning %s\nDebug %s\n", err->message, debug);

                         name = (gchar *)GST_MESSAGE_SRC_NAME(message);

                         g_print("Name of src %s\n", name ? name : "nil");
                         g_error_free(err);
                         g_free(debug);
                     }
                     break;

        case GST_MESSAGE_EOS:
                     g_print("End of stream\n");
                     g_main_loop_quit(app->loop);
                     break;

        case GST_MESSAGE_STATE_CHANGED:
                     break;

        default:
//					 g_print("got message %s\n", \
                             gst_message_type_get_name (GST_MESSAGE_TYPE (message)));
                     break;
    }

    return TRUE;
}

struct TouchScreenState {
    int x;
    int y;
    HU::TouchInfo::TOUCH_ACTION action;
    int action_recvd;
};

static const int MAX_TOUCH_SLOTS = 10;

struct TouchSlot {
    int trackingId = -1;
    int x = 0;
    int y = 0;
};

struct TouchLocation {
    uint32_t pointer_id;
    uint32_t x;
    uint32_t y;
};

// Fixed-capacity, trivially-copyable stand-in for a vector<TouchLocation> so
// it can be captured by value into the hu_queue_command lambda with no heap
// allocation.
struct TouchLocationSet {
    TouchLocation locations[MAX_TOUCH_SLOTS];
    int count = 0;
};

static TouchLocationSet single_touch_location(uint32_t x, uint32_t y) {
    TouchLocationSet set;
    set.locations[0] = {0, x, y};
    set.count = 1;
    return set;
}

// Builds the current location list from the slots marked active in
// `established`. If subjectSlot is a valid slot not yet marked established
// (a fresh press) or a slot about to be un-established (a release, still
// marked established by the caller), it is included as well and its
// position within the resulting list is written to *actionIndexOut.
static TouchLocationSet build_touch_locations(const TouchSlot* touchSlots, const bool* established, int subjectSlot, int* actionIndexOut) {
    TouchLocationSet set;
    for (int s = 0; s < MAX_TOUCH_SLOTS; s++) {
        if (!established[s] && s != subjectSlot)
            continue;
        if (s == subjectSlot && actionIndexOut)
            *actionIndexOut = set.count;
        set.locations[set.count] = {(uint32_t)s, (uint32_t)touchSlots[s].x, (uint32_t)touchSlots[s].y};
        set.count++;
    }
    return set;
}

static const char* touch_action_name(HU::TouchInfo::TOUCH_ACTION action) {
    switch (action) {
        case HU::TouchInfo::TOUCH_ACTION_PRESS: return "PRESS";
        case HU::TouchInfo::TOUCH_ACTION_RELEASE: return "RELEASE";
        case HU::TouchInfo::TOUCH_ACTION_DRAG: return "DRAG";
        default: return "UNKNOWN";
    }
}

static void aa_touch_event(HU::TouchInfo::TOUCH_ACTION action, const TouchLocationSet& locSet, int actionIndex, uint64_t ts) {

    printf("aa_touch_event(): action=%s actionIndex=%d numLocations=%d\n", touch_action_name(action), actionIndex, locSet.count);
    for (int i = 0; i < locSet.count; i++) {
        printf("  location pointer_id=%u x=%u y=%u\n", locSet.locations[i].pointer_id, locSet.locations[i].x, locSet.locations[i].y);
    }

    g_hu->hu_queue_command([action, locSet, actionIndex, ts](IHUConnectionThreadInterface& s)
    {
        HU::InputEvent inputEvent;
        inputEvent.set_timestamp(ts);
        HU::TouchInfo* touchEvent = inputEvent.mutable_touch();
        touchEvent->set_action(action);
        if (actionIndex >= 0) {
            touchEvent->set_action_index(actionIndex);
        }
        for (int i = 0; i < locSet.count; i++) {
            HU::TouchInfo::Location* touchLocation = touchEvent->add_location();
            touchLocation->set_x(locSet.locations[i].x);
            touchLocation->set_y(locSet.locations[i].y);
            touchLocation->set_pointer_id(locSet.locations[i].pointer_id);
        }

        /* Send touch event */

        int ret = s.hu_aap_enc_send_message(0, AA_CH_TOU, HU_INPUT_CHANNEL_MESSAGE::InputEvent, inputEvent);
        if (ret < 0) {
            printf("aa_touch_event(): hu_aap_enc_send() failed with (%d)\n", ret);
        }
    });
}

static uint64_t get_timestamp(struct input_event& ii)
{
    return ii.time.tv_sec * 1000000 + ii.time.tv_usec;
}
static void emit(int fd, int type, int code, int val)
{
  struct input_event ie;
  ie.type = type;
  ie.code = code;
  ie.value = val;
  ie.time.tv_sec = 0;
  ie.time.tv_usec = 0;
  write(fd, &ie, sizeof(ie));
}
/**
* Passes the keystroke to MZD by "ungrabbing" the kbd on key-down, simulating the same keystroke with uinput,
* then "re-grabbing" the kbd on key-up.
*/
void VideoOutput::pass_key_to_mzd(int type, int code, int val)
{
  if (val && ioctl(kbd_fd, EVIOCGRAB, 0) < 0)
  {
    fprintf(stderr, "EVIOCGRAB failed to release %s\n", EVENT_DEVICE_KBD);
  }

  emit(ui_fd, type, code, val);
  emit(ui_fd, EV_SYN, SYN_REPORT, 0);

  if(!val && ioctl(kbd_fd, EVIOCGRAB, 1) < 0)
  {
    fprintf(stderr, "EVIOCGRAB failed to grab %s\n", EVENT_DEVICE_KBD);
  }
}
void VideoOutput::input_thread_func()
{
    TouchScreenState mTouch {0,0,(HU::TouchInfo::TOUCH_ACTION)0,0};
    TouchSlot touchSlots[MAX_TOUCH_SLOTS];
    bool established[MAX_TOUCH_SLOTS] = {};
    int currentSlot = 0;
    bool usingMultitouchProtocol = false;
    int pendingPressSlots[MAX_TOUCH_SLOTS];
    int numPendingPress = 0;
    int pendingReleaseSlots[MAX_TOUCH_SLOTS];
    int numPendingRelease = 0;
    int maxfdPlus1 = std::max(std::max(touch_fd, kbd_fd), input_thread_quit_pipe_read) + 1;
    while (true)
    {
        fd_set set;
        int unblocked;

        FD_ZERO(&set);
        FD_SET(touch_fd, &set);
        FD_SET(kbd_fd, &set);
        FD_SET(input_thread_quit_pipe_read, &set);

        unblocked = select(maxfdPlus1, &set, NULL, NULL, NULL);

        if (unblocked == -1)
        {
            printf("Error in read...\n");
            g_main_loop_quit(gst_app.loop);
            break;
        }
        else if (unblocked > 0 && FD_ISSET(input_thread_quit_pipe_read, &set))
        {
            break;
        }

        struct input_event events[64];
        const size_t buffer_size = sizeof(events);

        if (FD_ISSET(touch_fd, &set))
        {
            ssize_t size = read(touch_fd, &events, buffer_size);

            if (size == 0 || size == -1)
                break;

            if (size < sizeof(input_event)) {
                printf("Error size when reading\n");
                g_main_loop_quit(gst_app.loop);
                break;
            }

            int num_chars = size / sizeof(input_event);
            for (int i=0;i < num_chars;i++)
            {
                auto& event = events[i];
                printf("touch_fd raw event: type=0x%02x code=0x%03x value=%d\n", event.type, event.code, event.value);
                switch (event.type)
                {
                    case EV_ABS:
                        switch (event.code) {
                            case ABS_MT_SLOT:
                                // if (!usingMultitouchProtocol)
                                    // printf("touch: detected ABS_MT_SLOT, switching to multitouch protocol\n");
                                usingMultitouchProtocol = true;
                                currentSlot = event.value;
                                if (currentSlot < 0)
                                    currentSlot = 0;
                                else if (currentSlot >= MAX_TOUCH_SLOTS)
                                    currentSlot = MAX_TOUCH_SLOTS - 1;
                                // printf("touch: ABS_MT_SLOT -> currentSlot=%d\n", currentSlot);
                                break;
                            case ABS_MT_TRACKING_ID:
                                // if (!usingMultitouchProtocol)
                                    // printf("touch: detected ABS_MT_TRACKING_ID, switching to multitouch protocol\n");
                                usingMultitouchProtocol = true;
                                if (event.value < 0) {
                                    // printf("touch: slot %d tracking id lost (release), had id=%d\n", currentSlot, touchSlots[currentSlot].trackingId);
                                    if (established[currentSlot] && numPendingRelease < MAX_TOUCH_SLOTS)
                                        pendingReleaseSlots[numPendingRelease++] = currentSlot;
                                    touchSlots[currentSlot].trackingId = -1;
                                } else {
                                    // printf("touch: slot %d tracking id assigned (press), id=%d\n", currentSlot, event.value);
                                    touchSlots[currentSlot].trackingId = event.value;
                                    if (numPendingPress < MAX_TOUCH_SLOTS)
                                        pendingPressSlots[numPendingPress++] = currentSlot;
                                }
                                break;
                            case ABS_MT_POSITION_X:
                                touchSlots[currentSlot].x = event.value * 800 /4095;
                                mTouch.x = touchSlots[currentSlot].x;
                                break;
                            case ABS_MT_POSITION_Y:
                                #if ASPECT_RATIO_FIX
                                touchSlots[currentSlot].y = event.value * 450/4095 + 15;
                                #else
                                touchSlots[currentSlot].y = event.value * 480/4095;
                                #endif
                                mTouch.y = touchSlots[currentSlot].y;
                                break;
                            default:
                                // printf("touch: unhandled EV_ABS code=0x%03x value=%d\n", event.code, event.value);
                                break;
                        }
                        break;
                    case EV_KEY:
                        // printf("touch: EV_KEY code=0x%03x (BTN_TOUCH=0x%03x) value=%d\n", event.code, BTN_TOUCH, event.value);
                        if (event.code == BTN_TOUCH) {
                            mTouch.action_recvd = 1;
                            if (event.value == 1) {
                                mTouch.action = HU::TouchInfo::TOUCH_ACTION_PRESS;
                            }
                            else {
                                mTouch.action = HU::TouchInfo::TOUCH_ACTION_RELEASE;
                            }
                        }
                        break;
                    case EV_SYN:
                        if (event.code != SYN_REPORT) {
                            // printf("touch: EV_SYN code=0x%03x (not SYN_REPORT), ignoring\n", event.code);
                            break;
                        }

                        // printf("touch: SYN_REPORT usingMultitouchProtocol=%d numPendingPress=%d numPendingRelease=%d\n",
                        //       usingMultitouchProtocol ? 1 : 0, numPendingPress, numPendingRelease);

                        if (usingMultitouchProtocol) {
                            uint64_t ts = get_timestamp(event);
                            bool hadPressOrRelease = numPendingPress > 0 || numPendingRelease > 0;

                            // "established" tracks which slots we have already announced to the
                            // phone as active pointers, so simultaneous presses/releases in one
                            // SYN_REPORT still get staged one at a time (each message only ever
                            // references pointers already introduced, or the one it's introducing).
                            for (int i = 0; i < numPendingRelease; i++) {
                                int slot = pendingReleaseSlots[i];
                                int actionIndex = -1;
                                TouchLocationSet locations = build_touch_locations(touchSlots, established, slot, &actionIndex);
                                aa_touch_event(HU::TouchInfo::TOUCH_ACTION_RELEASE, locations, actionIndex, ts);
                                established[slot] = false;
                            }
                            numPendingRelease = 0;

                            for (int i = 0; i < numPendingPress; i++) {
                                int slot = pendingPressSlots[i];
                                int actionIndex = -1;
                                TouchLocationSet locations = build_touch_locations(touchSlots, established, slot, &actionIndex);
                                aa_touch_event(HU::TouchInfo::TOUCH_ACTION_PRESS, locations, actionIndex, ts);
                                established[slot] = true;
                            }
                            numPendingPress = 0;

                            if (!hadPressOrRelease) {
                                TouchLocationSet locations = build_touch_locations(touchSlots, established, -1, nullptr);
                                if (locations.count > 0) {
                                    aa_touch_event(HU::TouchInfo::TOUCH_ACTION_DRAG, locations, -1, ts);
                                }
                            }
                        } else if (mTouch.action_recvd == 0) {
                            mTouch.action = HU::TouchInfo::TOUCH_ACTION_DRAG;
                            aa_touch_event(mTouch.action, single_touch_location((uint32_t)mTouch.x, (uint32_t)mTouch.y), -1, get_timestamp(event));
                        } else {
                            aa_touch_event(mTouch.action, single_touch_location((uint32_t)mTouch.x, (uint32_t)mTouch.y), -1, get_timestamp(event));
                            mTouch.action_recvd = 0;
                        }
                        break;
                }
            }
        }

        if (FD_ISSET(kbd_fd, &set))
        {
            ssize_t size = read(kbd_fd, &events, buffer_size);

            if (size == 0 || size == -1)
                break;

            if (size < sizeof(input_event)) {
                printf("Error size when reading\n");
                g_main_loop_quit(gst_app.loop);
                break;
            }

            int num_chars = size / sizeof(input_event);
            for (int i=0;i < num_chars;i++)
            {
                auto& event = events[i];
                if (event.type == EV_KEY && (event.value == 1 || event.value == 0))
                {
                    uint64_t timeStamp = get_timestamp(event);
                    uint32_t scanCode = 0;
                    int32_t scrollAmount = 0;
                    bool isPressed = (event.value == 1);
                    bool longPress = false;
                    AudioManagerClient::FocusType audioFocus = callbacks->audioFocus;
                    bool hasMediaAudioFocus = audioFocus == AudioManagerClient::FocusType::PERMANENT;
                    bool hasAudioFocus = audioFocus != AudioManagerClient::FocusType::NONE;

                    printf("Key code %i value %i\n", (int)event.code, (int)event.value);
                    switch (event.code)
                    {
                    case KEY_G:
                        printf("KEY_G\n");
                        scanCode = HUIB_MIC;
                        break;
                    //Make the music button play/pause
                    case KEY_E:
                        printf("KEY_E\n");
                        scanCode = HUIB_MUSIC;
                        break;
                    case KEY_LEFTBRACE:
                        printf("KEY_LEFTBRACE (next track with media focus: %i)\n",  hasMediaAudioFocus ? 1 : 0);
                        if(hasMediaAudioFocus)
                        {
                            scanCode = HUIB_NEXT;
                        }
                        else
                        {
                            pass_key_to_mzd(event.type, event.code, event.value);
                        }
                        break;
                    case KEY_RIGHTBRACE:
                        printf("KEY_RIGHTBRACE (prev track with media focus: %i)\n",  hasMediaAudioFocus ? 1 : 0);
                        if(hasMediaAudioFocus)
                        {
                            scanCode = HUIB_PREV;
                        }
                        else
                        {
                            pass_key_to_mzd(event.type, event.code, event.value);
                        }
                        break;
                    case KEY_BACKSPACE:
                        printf("KEY_BACKSPACE\n");
                        scanCode = HUIB_BACK;
                        break;
                    case KEY_ENTER:
                        printf("KEY_ENTER\n");
                        scanCode = HUIB_ENTER;
                        break;
                    case KEY_LEFT:
                        printf("KEY_LEFT\n");
                        scanCode = HUIB_LEFT;
                        break;
                    case KEY_RIGHT:
                        printf("KEY_RIGHT\n");
                        scanCode = HUIB_RIGHT;
                        break;
                    case KEY_UP:
                        printf("KEY_UP\n");
                        scanCode = HUIB_UP;
                        break;
                    case KEY_DOWN:
                        printf("KEY_DOWN\n");
                        scanCode = HUIB_DOWN;
                        break;
                    case KEY_N:
                        printf("KEY_N\n");
                        if (isPressed) {
                            scrollAmount = -1;
                        }
                        break;
                    case KEY_M:
                        printf("KEY_M\n");
                        if (isPressed) {
                            scrollAmount = 1;
                        }
                        break;
                    case KEY_HOME:
                        printf("KEY_HOME\n");
                        scanCode = HUIB_HOME;
                        break;
                    case KEY_R: // NAV
                        printf("KEY_R\n");
                        scanCode = HUIB_NAVIGATION;
                        break;
                    case KEY_Z: // CALL ANS
                        printf("KEY_Z\n");
                        scanCode = HUIB_PHONE;
                        break;
                    case KEY_X: // CALL END
                        printf("KEY_X\n");
#ifdef IOGRAB_DEBUG
                        if(hasMediaAudioFocus && isPressed && ioctl(kbd_fd, EVIOCGRAB, 0) < 0)
                        { // This is just for testing although it may be a useful feature if we polish it a little
                            fprintf(stderr, "EVIOCGRAB failed to ungrab %s\n", EVENT_DEVICE_KBD);
                        }
                        else
#endif
                        {	// we can do this since this button does nothing when not on a call
                            scanCode = HUIB_CALLEND;
                        }
                        break;
                    case KEY_T: // FAV
                        printf("KEY_T\n");
                        scanCode = HUIB_PLAYPAUSE;
                        break;
                    }
                    
                    if (isPressed)
                    {
                        pressScanCode = scanCode;
                        time(&pressedSince);
                    }
                    else
                    {
                        time_t now = time(NULL);
                        if (now - pressedSince >= 2)
                        {
                            if (pressScanCode == HUIB_PLAYPAUSE)
                            {
                                callbacks->releaseAudioFocus();
                            }
                            else if (pressScanCode == HUIB_BACK || pressScanCode == HUIB_CALLEND)
                            {
                                callbacks->releaseVideoFocus();
                            }
                            else if (pressScanCode == HUIB_HOME)
                            {
                                callbacks->takeVideoFocus();
                            }

                        }
                        pressScanCode = 0;
                    }

                    if (scanCode != 0 || scrollAmount != 0)
                    {
                        g_hu->hu_queue_command([timeStamp, scanCode, scrollAmount, isPressed, longPress](IHUConnectionThreadInterface& s)
                        {
                            HU::InputEvent inputEvent;
                            inputEvent.set_timestamp(timeStamp);
                            if (scanCode != 0)
                            {
                                HU::ButtonInfo* buttonInfo = inputEvent.mutable_button()->add_button();
                                buttonInfo->set_is_pressed(isPressed);
                                buttonInfo->set_meta(0);
                                buttonInfo->set_long_press(longPress);
                                buttonInfo->set_scan_code(scanCode);
                            }
                            if (scrollAmount != 0)
                            {
                                HU::RelativeInputEvent* rel = inputEvent.mutable_rel_event()->mutable_event();
                                rel->set_delta(scrollAmount);
                                rel->set_scan_code(HUIB_SCROLLWHEEL);
                            }
                            s.hu_aap_enc_send_message(0, AA_CH_TOU, HU_INPUT_CHANNEL_MESSAGE::InputEvent, inputEvent);
                        });
                    }
                }
            }
        }
    }
}



VideoOutput::VideoOutput(MazdaEventCallbacks* callbacks)
    : callbacks(callbacks)
{
    /* Open Touchscreen Device */
    touch_fd = open(EVENT_DEVICE_TS, O_RDONLY);

    if (touch_fd < 0) {
        fprintf(stderr, "%s is not a vaild device\n", EVENT_DEVICE_TS);
    }

    if (ioctl(touch_fd, EVIOCGRAB, 1) < 0)
    {
        fprintf(stderr, "EVIOCGRAB failed on %s\n", EVENT_DEVICE_TS);
    }

    kbd_fd = open(EVENT_DEVICE_KBD, O_RDONLY);

    if (kbd_fd < 0)
    {
        fprintf(stderr, "%s is not a vaild device\n", EVENT_DEVICE_KBD);
    }

    if (ioctl(kbd_fd, EVIOCGRAB, 1) < 0)
    {
        fprintf(stderr, "EVIOCGRAB failed on %s\n", EVENT_DEVICE_KBD);
    }

    ui_fd = open(EVENT_DEVICE_UI, O_WRONLY | O_NONBLOCK);

    if (ui_fd < 0)
    {
        fprintf(stderr, "%s is not a vaild device\n", EVENT_DEVICE_UI);
    }

    if (ioctl(ui_fd, UI_SET_EVBIT, EV_KEY) < 0)
    {
        fprintf(stderr, "UI_SET_EVBIT failed on %s\n", EV_KEY);
    }
    if (ioctl(ui_fd, UI_SET_KEYBIT, KEY_LEFTBRACE) < 0)
    {
        fprintf(stderr, "UI_SET_KEYBIT failed on %s\n", KEY_LEFTBRACE);
    }
    if (ioctl(ui_fd, UI_SET_KEYBIT, KEY_RIGHTBRACE) < 0)
    {
        fprintf(stderr, "UI_SET_KEYBIT failed on %s\n", KEY_RIGHTBRACE);
    }
    if (ioctl(ui_fd, UI_SET_KEYBIT, KEY_E) < 0)
    {
        fprintf(stderr, "UI_SET_KEYBIT failed on %s\n", KEY_E);
    }
    struct uinput_user_dev uidev;
    memset(&uidev, 0, sizeof(uidev));
    snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "mzd-uinput");
    uidev.id.bustype = BUS_USB;
    uidev.id.vendor  = 0x1;
    uidev.id.product = 0x1;
    uidev.id.version = 1;

    if(write(ui_fd, &uidev, sizeof(uidev)) < 0)
    {
        fprintf(stderr, "Write uidev failed");
    }

    if (ioctl(ui_fd, UI_DEV_CREATE) < 0)
    {
        fprintf(stderr, "UI_DEV_CREATE failed on %s\n", EVENT_DEVICE_UI);
    }

    int quitpiperw[2];
    if (pipe(quitpiperw) < 0) {
        fprintf(stderr, "Pipe failed");
    }
    input_thread_quit_pipe_read = quitpiperw[0];
    input_thread_quit_pipe_write = quitpiperw[1];

    input_thread = std::thread([this](){ input_thread_func(); } );
    //Drop caches before staing new video
    sync();
    std::ofstream ofs("/proc/sys/vm/drop_caches");
    ofs << "3" << std::endl;
    //if we have ASPECT_RATIO_FIX, cut off the bottom black bar
    const char* vid_pipeline_launch = "appsrc name=mysrc is-live=true block=false max-latency=1000000 do-timestamp=true ! queue ! h264parse ! vpudec low-latency=true framedrop=true framedrop-level-mask=0x200 frame-plus=1 ! mfw_isink name=mysink "
    #if ASPECT_RATIO_FIX
    "axis-left=0 axis-top=-20 disp-width=800 disp-height=520"
    #else
    "axis-left=0 axis-top=0 disp-width=800 disp-height=480"
    #endif
    " max-lateness=1000000000 sync=false async=false";

    GError* error = nullptr;
    vid_pipeline = gst_parse_launch(vid_pipeline_launch, &error);

    if (error != NULL) {
        printf("could not construct pipeline: %s\n", error->message);
        g_clear_error (&error);
    }

    GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(vid_pipeline));
    gst_bus_add_watch(bus, (GstBusFunc)bus_callback, nullptr);
    gst_object_unref(bus);

    vid_src = GST_APP_SRC(gst_bin_get_by_name (GST_BIN (vid_pipeline), "mysrc"));
    vid_sink = GST_ELEMENT(gst_bin_get_by_name (GST_BIN (vid_pipeline), "mysink"));

    gst_app_src_set_stream_type(vid_src, GST_APP_STREAM_TYPE_STREAM);

    gst_element_set_state((GstElement*)vid_pipeline, GST_STATE_PLAYING);
}

VideoOutput::~VideoOutput()
{
    gst_element_set_state((GstElement*)vid_pipeline, GST_STATE_NULL);
    //mfw_isink releases its IPU surface asynchronously - block until the NULL
    //transition actually completes, otherwise the surface can leak across
    //repeated video focus cycles until the IPU device runs out ("createVideoSurface:
    //max surfaces on device support on device1 exceeded!")
    gst_element_get_state((GstElement*)vid_pipeline, nullptr, nullptr, GST_CLOCK_TIME_NONE);

    //data we write doesn't matter, wake up touch polling thread
    write(input_thread_quit_pipe_write, &input_thread_quit_pipe_write, sizeof(input_thread_quit_pipe_write));

    printf("waiting for input_thread\n");
    input_thread.join();

    ioctl(ui_fd, UI_DEV_DESTROY);
    close(ui_fd);
    close(touch_fd);
    close(kbd_fd);
    close(input_thread_quit_pipe_write);
    close(input_thread_quit_pipe_read);


    gst_object_unref(vid_pipeline);
    gst_object_unref(vid_src);
    gst_object_unref(vid_sink);
}

void VideoOutput::MediaPacket(uint64_t timestamp, const byte *buf, int len)
{
    GstBuffer * buffer = gst_buffer_new_and_alloc(len);
    memcpy(GST_BUFFER_DATA(buffer), buf, len);
    int ret = gst_app_src_push_buffer(vid_src, buffer);
    if(ret !=  GST_FLOW_OK){
        printf("push buffer returned %d for %d bytes \n", ret, len);
    }
}
