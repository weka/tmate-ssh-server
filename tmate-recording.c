#include <nats/nats.h>
#include <signal.h>
#include "tmate.h"

natsConnection *nc = NULL;
natsOptions *natsOpt = NULL;

void tmate_recording_init(struct tmate_session *session) {
    pid_t pid;

    if(tmate_settings->nats_url == NULL) {
        tmate_info("tmate recording is disabled");
        return;
    }
    if (session == NULL) {
        tmate_debug("skipping recording for null session");
        return;
    }

    tmate_info("enabling tmate recording session");

    if (pipe(session->recording_pipe) == -1)
		tmate_fatal("failed to open recording pipe");

	if((pid = fork()) < 0)
		tmate_fatal("can't fork");

	if (pid > 0) {
        signal(SIGPIPE, SIG_IGN); // do not handle SIGPIPE
        close(session->recording_pipe[0]); // we don't use read pipe in parent
        return;
    }


	/* Child recording process */	

    signal(SIGPIPE, SIG_IGN); // do not handle SIGPIPE
    close(session->recording_pipe[1]); // we don't use write pipe in child

    natsOptions_Create(&natsOpt);
    natsOptions_SetMaxReconnect(natsOpt, -1);
    natsOptions_SetURL(natsOpt, tmate_settings->nats_url);
    natsOptions_SetRetryOnFailedConnect(natsOpt, true, NULL, NULL);

    natsStatus ret;

    ret = natsConnection_Connect(&nc, natsOpt);
    if (ret != NATS_OK) {
        close(session->recording_pipe[0]);
        tmate_fatal("tmate recording setup is failed, code: %d", ret);
    }

    tmate_info("recording started");

    char inbuf[TMATE_MAX_MESSAGE_SIZE]; 
	int nbytes;
		
    while ((nbytes = read(session->recording_pipe[0], inbuf, TMATE_MAX_MESSAGE_SIZE)) > 0) {
        tmate_recording_send(session, inbuf, nbytes);
	}

    tmate_info("recording pipe closed");

    tmate_recording_end(session);

    exit(0);
}

void tmate_recording_send(struct tmate_session *session, const void *buf, int len) {
    if (session->recording_pipe[1] == -1) {
        tmate_debug("recoding is disabled");
        return;
    }

    if (nc == NULL) {
        /* parent process call */
        tmate_debug("sending to recording pipe %d bytes", len);

		if (write(session->recording_pipe[1], buf, len) != len) {
			tmate_fatal("recoring write: %d", errno);
		}
        return;
    }

    /* child process publishing */

    char *subject = NULL;
    asprintf(&subject, "%s.%s", tmate_settings->nats_topic, session->session_token);

    if(subject == NULL) {
        tmate_fatal("allocate recording topic returned zero result");
    }

    log_debug("recording %d bytes of session to %s.%s", len, tmate_settings->nats_topic, session->obfuscated_session_token);

    natsStatus ret;

    ret = natsConnection_Publish(nc, subject, buf, len);
    if(ret != NATS_OK) {
        close(session->recording_pipe[0]);
        tmate_fatal("nats publish error code: %d", ret);
    }

    free(subject);
}

void tmate_recording_end(struct tmate_session *session) {
    if(nc == NULL) {
        // should be null for parent process
        tmate_debug("no nats connection, skipping recording end");
        return;
    }

    if(session == NULL) {
        tmate_debug("null session, skipping recording end");
        return;
    }

    char *subject = NULL;
    asprintf(&subject, "%s.%s.end",tmate_settings->nats_topic, session->session_token);

    if(subject == NULL) {
        tmate_fatal("allocate recording topic returned zero result");
    }

    tmate_info("sending end of session to %s.%s", tmate_settings->nats_topic, session->obfuscated_session_token);

    natsStatus ret;

    ret = natsConnection_PublishString(nc, subject, "EOF");
    if(ret != NATS_OK) {
        tmate_fatal("nats publish error code: %d", ret);
    }

// we want be realiable so can do flushing forever after end of session
flush:
    ret = natsConnection_Flush(nc);
    if (ret != NATS_OK) {
        tmate_debug("nats flush error code: %d", ret);
        nats_Sleep(1000);
        goto flush;
    }

    free(subject);

    tmate_debug("closing nats connection");
    natsConnection_Close(nc);
    natsConnection_Destroy(nc);
}

void tmate_recording_close(struct tmate_session *session) {
    // closing opened pipes if any
    if(session->recording_pipe[0]) {
        close(session->recording_pipe[0]);
    }
    if(session->recording_pipe[1]) {
        close(session->recording_pipe[1]);
    }

    tmate_debug("tmate recording closed");
}