#include <nats/nats.h>
#include "tmate.h"

natsConnection *nc = NULL;
natsOptions *natsOpt = NULL;

void tmate_init_recording(struct tmate_session *session) {
    if(tmate_settings->nats_url == NULL) {
        tmate_info("tmate recoding is disabled");
        return;
    }

    tmate_info("enabling tmate recording session %s", session->obfuscated_session_token);

    natsOptions_Create(&natsOpt);
    natsOptions_SetMaxReconnect(natsOpt, -1);
    natsOptions_SetURL(natsOpt, tmate_settings->nats_url);

    natsStatus ret;

    ret = natsConnection_Connect(&nc, natsOpt);
    if (ret != NATS_OK) {
        nc = NULL;
        tmate_fatal("tmate recoding setup is failed, code: %d", ret);
    }
}

void tmate_send_record(struct tmate_session *session, const void *buf, int len) {
    if(nc == NULL) {
        log_debug("tmate recording is disabled");
        return;
    }

    log_debug("recoding %d bytes of session %s", len, session->obfuscated_session_token);

    char *topic = NULL;
    asprintf(&topic, "ingest.tmate.recording.%s", session->session_token);

    if(topic == NULL) {
        return;
    }

    natsStatus ret;

    ret = natsConnection_Publish(nc, topic, buf, len);
    if(ret != NATS_OK) {
        tmate_fatal("nats publish error code: %d", ret);
    }

    free(topic);
}

extern int natsConnection_Fd(natsConnection *nc);

int tmate_recording_fd() {
    if (nc == NULL) {
        return -1;
    }

    return natsConnection_Fd(nc);
}