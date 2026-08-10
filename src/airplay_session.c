#include "airplay_session.h"
#include "airplay_rtp.h"

#include <string.h>
#include <unistd.h>

void airplay_session_init(airplay_session *sess)
{
    memset(sess, 0, sizeof(*sess));
    sess->audio_fd = -1;
    sess->control_fd = -1;
    sess->encrypted = 1; /* real classic-AirPlay senders always encrypt - see
                           * airplay_rtsp.c's parse_announce_sdp for the one
                           * real (rare) case this gets corrected to 0 */
    sess->volume_linear = 1.0f; /* full volume until a real SET_PARAMETER says otherwise */
}

void airplay_session_close(airplay_session *sess)
{
    airplay_rtp_stop(sess);

    if (sess->output != NULL) {
        coreaudio_output_close(sess->output);
        sess->output = NULL;
    }
    if (sess->decoder != NULL) {
        alac_free(sess->decoder);
        sess->decoder = NULL;
    }
    if (sess->audio_fd >= 0) { close(sess->audio_fd); sess->audio_fd = -1; }
    if (sess->control_fd >= 0) { close(sess->control_fd); sess->control_fd = -1; }
}
