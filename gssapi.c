/*
 * OpenConnect (SSL + DTLS) VPN client
 *
 * Copyright © 2008-2015 Intel Corporation.
 *
 * Author: David Woodhouse <dwmw2@infradead.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * version 2.1, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 */

#include <config.h>

#include <errno.h>
#include <string.h>

#include "openconnect-internal.h"

static void print_gss_err(struct openconnect_info *vpninfo, const char *where,
			  gss_OID mech, OM_uint32 err_maj, OM_uint32 err_min)
{
	OM_uint32 major, minor, msg_ctx = 0;
	gss_buffer_desc status;

	do {
		major = gss_display_status(&minor, err_maj, GSS_C_GSS_CODE,
					   mech, &msg_ctx, &status);
		if (GSS_ERROR(major))
			break;
		vpn_progress(vpninfo, PRG_ERR, "%s: %s\n", where, (char *)status.value);
		gss_release_buffer(&minor, &status);
	} while (msg_ctx);

	msg_ctx = 0;
	do {
		major = gss_display_status(&minor, err_min, GSS_C_MECH_CODE,
					   mech, &msg_ctx, &status);
		if (GSS_ERROR(major))
			break;
		vpn_progress(vpninfo, PRG_ERR, "%s: %s\n", where, (char *)status.value);
		gss_release_buffer(&minor, &status);
	} while (msg_ctx);
}

static const char spnego_OID[] = "\x2b\x06\x01\x05\x05\x02";
static const gss_OID_desc gss_mech_spnego = {
        6,
	(void *)&spnego_OID
};

static int gssapi_setup(struct openconnect_info *vpninfo, struct http_auth_state *auth_state,
			const char *service, int proxy)
{
	OM_uint32 major, minor;
	gss_buffer_desc token = GSS_C_EMPTY_BUFFER;
	char *name;

	if (asprintf(&name, "%s@%s", service,
		     proxy ? vpninfo->proxy : vpninfo->hostname) == -1)
		return -ENOMEM;
	token.length = strlen(name);
	token.value = name;

	major = gss_import_name(&minor, &token, (gss_OID)GSS_C_NT_HOSTBASED_SERVICE,
				&auth_state->gss_target_name);
	free(name);
	if (GSS_ERROR(major)) {
		vpn_progress(vpninfo, PRG_ERR,
			     _("Error importing GSSAPI name for authentication:\n"));
		print_gss_err(vpninfo, "gss_import_name()", GSS_C_NO_OID, major, minor);
		return -EIO;
	}
	return 0;
}

#define GSSAPI_CONTINUE	2
#define GSSAPI_COMPLETE	3

int gssapi_authorization(struct openconnect_info *vpninfo, int proxy,
			 struct http_auth_state *auth_state,
			 struct oc_text_buf *hdrbuf)
{
	OM_uint32 major, minor;
	gss_buffer_desc in = GSS_C_EMPTY_BUFFER;
	gss_buffer_desc out = GSS_C_EMPTY_BUFFER;
	gss_OID mech = GSS_C_NO_OID;

	if (auth_state->state == AUTH_AVAILABLE && gssapi_setup(vpninfo, auth_state, "HTTP", proxy)) {
		auth_state->state = AUTH_FAILED;
		return -EIO;
	}

	if (auth_state->challenge && *auth_state->challenge) {
		int len = -EINVAL;
		in.value = openconnect_base64_decode(&len, auth_state->challenge);
		if (!in.value)
			return len;
		in.length = len;
	} else if (auth_state->state > AUTH_AVAILABLE) {
		/* This indicates failure. We were trying, but got an empty
		   'Proxy-Authorization: Negotiate' header back from the server
		   implying that we should start again... */
		goto fail_gssapi;
	}

	major = gss_init_sec_context(&minor, GSS_C_NO_CREDENTIAL,
				     &auth_state->gss_context,
				     auth_state->gss_target_name,
				     (gss_OID)&gss_mech_spnego,
				     GSS_C_MUTUAL_FLAG, GSS_C_INDEFINITE,
				     GSS_C_NO_CHANNEL_BINDINGS, &in,
				     &mech, &out, NULL, NULL);
	if (in.value)
		free(in.value);

	if (major == GSS_S_COMPLETE)
		auth_state->state = GSSAPI_COMPLETE;
	else if (major == GSS_S_CONTINUE_NEEDED)
		auth_state->state = GSSAPI_CONTINUE;
	else {
		vpn_progress(vpninfo, PRG_ERR,
			     _("Error generating GSSAPI response:\n"));
		print_gss_err(vpninfo, "gss_init_sec_context()", mech, major, minor);
	fail_gssapi:
		auth_state->state = AUTH_FAILED;
		cleanup_gssapi_auth(vpninfo, auth_state);
		/* If we were *trying*, then -EAGAIN. Else -ENOENT to let another
		   auth method try without having to reconnect first. */
		return in.value ? -EAGAIN : -ENOENT;
	}
	buf_append(hdrbuf, "%sAuthorization: Negotiate ", proxy ? "Proxy-" : "");
	buf_append_base64(hdrbuf, out.value, out.length);
	buf_append(hdrbuf, "\r\n");

	gss_release_buffer(&minor, &out);
	if (!auth_state->challenge) {
		if (proxy)
			vpn_progress(vpninfo, PRG_INFO,
				     _("Attempting GSSAPI authentication to proxy\n"));
		else
			vpn_progress(vpninfo, PRG_INFO,
				     _("Attempting GSSAPI authentication to server '%s'\n"),
				     vpninfo->hostname);
	}

	return 0;
}

/* auth_state is NULL when called from socks_gssapi_auth() */
void cleanup_gssapi_auth(struct openconnect_info *vpninfo,
			 struct http_auth_state *auth_state)
{
	OM_uint32 minor;

	if (auth_state->gss_target_name != GSS_C_NO_NAME)
		gss_release_name(&minor, &auth_state->gss_target_name);

	if (auth_state->gss_context != GSS_C_NO_CONTEXT)
		gss_delete_sec_context(&minor, &auth_state->gss_context, GSS_C_NO_BUFFER);

	/* Shouldn't be necessary, but make sure... */
	auth_state->gss_target_name = GSS_C_NO_NAME;
	auth_state->gss_context = GSS_C_NO_CONTEXT;
}

int socks_gssapi_auth(struct openconnect_info *vpninfo)
{
	gss_buffer_desc in = GSS_C_EMPTY_BUFFER;
	gss_buffer_desc out = GSS_C_EMPTY_BUFFER;
	gss_OID mech = GSS_C_NO_OID;
	OM_uint32 major, minor;
	unsigned char *pktbuf;
	int i;
	int ret = -EIO;
	struct http_auth_state *auth_state = &vpninfo->proxy_auth[AUTH_TYPE_GSSAPI];

	if (gssapi_setup(vpninfo, auth_state, "rcmd", 1))
		return -EIO;

	pktbuf = malloc(65538);
	if (!pktbuf)
		return -ENOMEM;
	while (1) {
		major = gss_init_sec_context(&minor, GSS_C_NO_CREDENTIAL, &auth_state->gss_context,
					     auth_state->gss_target_name, (gss_OID)&gss_mech_spnego,
					     GSS_C_MUTUAL_FLAG | GSS_C_REPLAY_FLAG | GSS_C_DELEG_FLAG | GSS_C_SEQUENCE_FLAG,
					     GSS_C_INDEFINITE, GSS_C_NO_CHANNEL_BINDINGS, &in, &mech,
					     &out, NULL, NULL);
		in.value = NULL;
		if (major == GSS_S_COMPLETE) {
			/* If we still have a token to send, send it. */
			if (!out.length) {
				vpn_progress(vpninfo, PRG_DEBUG,
					     _("GSSAPI authentication completed\n"));
				gss_release_buffer(&minor, &out);
				ret = 0;
				break;
			}
		} else if (major != GSS_S_CONTINUE_NEEDED) {
			print_gss_err(vpninfo, "gss_init_sec_context()", mech, major, minor);
			break;
		}
		if (out.length > 65535) {
			vpn_progress(vpninfo, PRG_ERR,
				     _("GSSAPI token too large (%zd bytes)\n"),
				     out.length);
			break;
		}

		pktbuf[0] = 1; /* ver */
		pktbuf[1] = 1; /* mtyp */
		store_be16(pktbuf + 2, out.length);
		memcpy(pktbuf + 4, out.value, out.length);

		free(out.value);

		vpn_progress(vpninfo, PRG_TRACE,
			     _("Sending GSSAPI token of %zu bytes\n"), out.length + 4);

		i = vpninfo->ssl_write(vpninfo, (void *)pktbuf, out.length + 4);
		if (i < 0) {
			vpn_progress(vpninfo, PRG_ERR,
				     _("Failed to send GSSAPI authentication token to proxy: %s\n"),
				     strerror(-i));
			break;
		}

		i = vpninfo->ssl_read(vpninfo, (void *)pktbuf, 4);
		if (i < 0) {
			vpn_progress(vpninfo, PRG_ERR,
				     _("Failed to receive GSSAPI authentication token from proxy: %s\n"),
				     strerror(-i));
			break;
		}
		if (pktbuf[1] == 0xff) {
			vpn_progress(vpninfo, PRG_ERR,
				     _("SOCKS server reported GSSAPI context failure\n"));
			break;
		} else if (pktbuf[1] != 1) {
			vpn_progress(vpninfo, PRG_ERR,
				     _("Unknown GSSAPI status response (0x%02x) from SOCKS server\n"),
				     pktbuf[1]);
			break;
		}
		in.length = load_be16(pktbuf + 2);
		in.value = pktbuf;

		if (!in.length) {
			vpn_progress(vpninfo, PRG_DEBUG,
				     _("GSSAPI authentication completed\n"));
			ret = 0;
			break;
		}

		i = vpninfo->ssl_read(vpninfo, (void *)pktbuf, in.length);
		if (i < 0) {
			vpn_progress(vpninfo, PRG_ERR,
				     _("Failed to receive GSSAPI authentication token from proxy: %s\n"),
				     strerror(-i));
			break;
		}
		vpn_progress(vpninfo, PRG_TRACE, _("Got GSSAPI token of %zu bytes: %02x %02x %02x %02x\n"),
			     in.length, pktbuf[0], pktbuf[1], pktbuf[2], pktbuf[3]);
	}

	if (!ret) {
		ret = -EIO;

		pktbuf[0] = 0;
		in.value = pktbuf;
		in.length = 1;

		major = gss_wrap(&minor, auth_state->gss_context, 0,
				 GSS_C_QOP_DEFAULT, &in, NULL, &out);
		if (major != GSS_S_COMPLETE) {
			print_gss_err(vpninfo, "gss_wrap()", mech, major, minor);
			goto err;
		}

		pktbuf[0] = 1;
		pktbuf[1] = 2;
		store_be16(pktbuf + 2, out.length);
		memcpy(pktbuf + 4, out.value, out.length);

		free(out.value);

		vpn_progress(vpninfo, PRG_TRACE,
			     _("Sending GSSAPI protection negotiation of %zu bytes\n"), out.length + 4);

		i = vpninfo->ssl_write(vpninfo, (void *)pktbuf, out.length + 4);
		if (i < 0) {
			vpn_progress(vpninfo, PRG_ERR,
				     _("Failed to send GSSAPI protection response to proxy: %s\n"),
				     strerror(-i));
			goto err;
		}

		i = vpninfo->ssl_read(vpninfo, (void *)pktbuf, 4);
		if (i < 0) {
			vpn_progress(vpninfo, PRG_ERR,
				     _("Failed to receive GSSAPI protection response from proxy: %s\n"),
				     strerror(-i));
			goto err;
		}
		in.length = load_be16(pktbuf + 2);
		in.value = pktbuf;

		i = vpninfo->ssl_read(vpninfo, (void *)pktbuf, in.length);
		if (i < 0) {
			vpn_progress(vpninfo, PRG_ERR,
				     _("Failed to receive GSSAPI protection response from proxy: %s\n"),
				     strerror(-i));
			goto err;
		}
		vpn_progress(vpninfo, PRG_TRACE,
			     _("Got GSSAPI protection response of %zu bytes: %02x %02x %02x %02x\n"),
			     in.length, pktbuf[0], pktbuf[1], pktbuf[2], pktbuf[3]);

		major = gss_unwrap(&minor, auth_state->gss_context, &in, &out, NULL, GSS_C_QOP_DEFAULT);
		if (major != GSS_S_COMPLETE) {
			print_gss_err(vpninfo, "gss_unwrap()", mech, major, minor);
			goto err;
		}
		if (out.length != 1) {
			vpn_progress(vpninfo, PRG_ERR,
				     _("Invalid GSSAPI protection response from proxy (%zu bytes)\n"),
				     out.length);
			gss_release_buffer(&minor, &out);
			goto err;
		}
		i = *(char *)out.value;
		gss_release_buffer(&minor, &out);
		if (i == 1) {
			vpn_progress(vpninfo, PRG_ERR,
				     _("SOCKS proxy demands message integrity, which is not supported\n"));
			goto err;
		} else if (i == 2) {
			vpn_progress(vpninfo, PRG_ERR,
				     _("SOCKS proxy demands message confidentiality, which is not supported\n"));
			goto err;
		} else if (i) {
			vpn_progress(vpninfo, PRG_ERR,
				     _("SOCKS proxy demands protection unknown type 0x%02x\n"),
				     (unsigned char)i);
			goto err;
		}
		ret = 0;
	}
 err:
	cleanup_gssapi_auth(vpninfo, NULL);
	free(pktbuf);

	return ret;
}
