/*-
 * Copyright (c) 2006 Verdens Gang AS
 * Copyright (c) 2006-2015 Varnish Software AS
 * All rights reserved.
 *
 * Author: Poul-Henning Kamp <phk@phk.freebsd.dk>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * The director implementation for VCL backends.
 *
 */

#include "config.h"

#include <stdlib.h>

#include "cache_varnishd.h"

#include "vtcp.h"
#include "vtim.h"
#include "waiter/waiter.h"

#include "cache_director.h"
#include "cache_backend.h"
#include "cache_tcp_pool.h"
#include "cache_transport.h"
#include "http1/cache_http1.h"

#include "VSC_vbe.h"

/*--------------------------------------------------------------------*/

static const char * const vbe_proto_ident = "HTTP Backend";

static VTAILQ_HEAD(, backend) backends = VTAILQ_HEAD_INITIALIZER(backends);
static VTAILQ_HEAD(, backend) cool_backends =
    VTAILQ_HEAD_INITIALIZER(cool_backends);
static struct lock backends_mtx;

/*--------------------------------------------------------------------*/

#define FIND_TMO(tmx, dst, bo, be)					\
	do {								\
		CHECK_OBJ_NOTNULL(bo, BUSYOBJ_MAGIC);			\
		dst = bo->tmx;						\
		if (dst == 0.0)						\
			dst = be->tmx;					\
		if (dst == 0.0)						\
			dst = cache_param->tmx;				\
	} while (0)

/*--------------------------------------------------------------------
 * Get a connection to the backend
 */

static struct vtp *
vbe_dir_getfd(struct worker *wrk, struct backend *bp, struct busyobj *bo,
    unsigned force_fresh)
{
	struct vtp *vtp;
	double tmod;
	char abuf1[VTCP_ADDRBUFSIZE], abuf2[VTCP_ADDRBUFSIZE];
	char pbuf1[VTCP_PORTBUFSIZE], pbuf2[VTCP_PORTBUFSIZE];

	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	CHECK_OBJ_NOTNULL(bo, BUSYOBJ_MAGIC);
	CHECK_OBJ_NOTNULL(bp, BACKEND_MAGIC);
	AN(bp->vsc);

	if (!VDI_Healthy(bp->director, NULL)) {
		VSLb(bo->vsl, SLT_FetchError,
		     "backend %s: unhealthy", bp->director->display_name);
		// XXX: per backend stats ?
		VSC_C_main->backend_unhealthy++;
		return (NULL);
	}

	if (bp->max_connections > 0 && bp->n_conn >= bp->max_connections) {
		VSLb(bo->vsl, SLT_FetchError,
		     "backend %s: busy", bp->director->display_name);
		// XXX: per backend stats ?
		VSC_C_main->backend_busy++;
		return (NULL);
	}

	AZ(bo->htc);
	bo->htc = WS_Alloc(bo->ws, sizeof *bo->htc);
	if (bo->htc == NULL) {
		VSLb(bo->vsl, SLT_FetchError, "out of workspace");
		/* XXX: counter ? */
		return (NULL);
	}
	bo->htc->doclose = SC_NULL;

	FIND_TMO(connect_timeout, tmod, bo, bp);
	vtp = VTP_Get(bp->tcp_pool, tmod, wrk, force_fresh);
	if (vtp == NULL) {
		VSLb(bo->vsl, SLT_FetchError,
		     "backend %s: fail", bp->director->display_name);
		// XXX: Per backend stats ?
		VSC_C_main->backend_fail++;
		bo->htc = NULL;
		return (NULL);
	}

	assert(vtp->fd >= 0);
	AN(vtp->addr);

	Lck_Lock(&bp->mtx);
	bp->n_conn++;
	bp->vsc->conn++;
	bp->vsc->req++;
	Lck_Unlock(&bp->mtx);

	if (bp->proxy_header != 0)
		VPX_Send_Proxy(vtp->fd, bp->proxy_header, bo->sp);

	VTCP_myname(vtp->fd, abuf1, sizeof abuf1, pbuf1, sizeof pbuf1);
	VTCP_hisname(vtp->fd, abuf2, sizeof abuf2, pbuf2, sizeof pbuf2);
	VSLb(bo->vsl, SLT_BackendOpen, "%d %s %s %s %s %s",
	    vtp->fd, bp->director->display_name, abuf2, pbuf2, abuf1, pbuf1);

	INIT_OBJ(bo->htc, HTTP_CONN_MAGIC);
	bo->htc->priv = vtp;
	bo->htc->rfd = &vtp->fd;
	FIND_TMO(first_byte_timeout,
	    bo->htc->first_byte_timeout, bo, bp);
	FIND_TMO(between_bytes_timeout,
	    bo->htc->between_bytes_timeout, bo, bp);
	return (vtp);
}

static unsigned v_matchproto_(vdi_healthy_f)
vbe_dir_healthy(const struct director *d, const struct busyobj *bo,
    double *changed)
{
	struct backend *be;

	CHECK_OBJ_NOTNULL(d, DIRECTOR_MAGIC);
	CHECK_OBJ_ORNULL(bo, BUSYOBJ_MAGIC);
	CAST_OBJ_NOTNULL(be, d->priv, BACKEND_MAGIC);
	return (VDI_Healthy(be->director, changed));
}

static unsigned v_matchproto_(vdi_uptime_f)
vbe_dir_uptime(const struct director *d, const struct busyobj *bo,
    double *changed, double *load)
{
	struct backend *be;

	CHECK_OBJ_NOTNULL(d, DIRECTOR_MAGIC);
	CAST_OBJ_NOTNULL(be, d->priv, BACKEND_MAGIC);

	if (load != NULL)
		*load = be->n_conn;
	return (VBE_Healthy(be, changed));
}

static void v_matchproto_(vdi_finish_f)
vbe_dir_finish(const struct director *d, struct worker *wrk,
    struct busyobj *bo)
{
	struct backend *bp;
	struct vtp *vtp;

	CHECK_OBJ_NOTNULL(d, DIRECTOR_MAGIC);
	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	CHECK_OBJ_NOTNULL(bo, BUSYOBJ_MAGIC);
	CAST_OBJ_NOTNULL(bp, d->priv, BACKEND_MAGIC);

	CHECK_OBJ_NOTNULL(bo->htc, HTTP_CONN_MAGIC);
	CAST_OBJ_NOTNULL(vtp, bo->htc->priv, VTP_MAGIC);
	bo->htc->priv = NULL;
	if (vtp->state != VTP_STATE_USED)
		assert(bo->htc->doclose == SC_TX_PIPE ||
			bo->htc->doclose == SC_RX_TIMEOUT);
	if (bo->htc->doclose != SC_NULL || bp->proxy_header != 0) {
		VSLb(bo->vsl, SLT_BackendClose, "%d %s", vtp->fd,
		    bp->director->display_name);
		VTP_Close(&vtp);
		AZ(vtp);
		Lck_Lock(&bp->mtx);
	} else {
		assert (vtp->state == VTP_STATE_USED);
		VSLb(bo->vsl, SLT_BackendReuse, "%d %s", vtp->fd,
		    bp->director->display_name);
		Lck_Lock(&bp->mtx);
		VSC_C_main->backend_recycle++;
		VTP_Recycle(wrk, &vtp);
	}
	assert(bp->n_conn > 0);
	bp->n_conn--;
	bp->vsc->conn--;
#define ACCT(foo)	bp->vsc->foo += bo->acct.foo;
#include "tbl/acct_fields_bereq.h"
	Lck_Unlock(&bp->mtx);
	bo->htc = NULL;
}

static int v_matchproto_(vdi_gethdrs_f)
vbe_dir_gethdrs(const struct director *d, struct worker *wrk,
    struct busyobj *bo)
{
	int i, extrachance = 1;
	struct backend *bp;
	struct vtp *vtp;

	CHECK_OBJ_NOTNULL(d, DIRECTOR_MAGIC);
	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	CHECK_OBJ_NOTNULL(bo, BUSYOBJ_MAGIC);
	CAST_OBJ_NOTNULL(bp, d->priv, BACKEND_MAGIC);

	/*
	 * Now that we know our backend, we can set a default Host:
	 * header if one is necessary.  This cannot be done in the VCL
	 * because the backend may be chosen by a director.
	 */
	if (!http_GetHdr(bo->bereq, H_Host, NULL) && bp->hosthdr != NULL)
		http_PrintfHeader(bo->bereq, "Host: %s", bp->hosthdr);

	do {
		vtp = vbe_dir_getfd(wrk, bp, bo, extrachance == 0);
		if (vtp == NULL)
			return (-1);
		AN(bo->htc);
		if (vtp->state != VTP_STATE_STOLEN)
			extrachance = 0;

		i = V1F_SendReq(wrk, bo, &bo->acct.bereq_hdrbytes, 0);

		if (vtp->state != VTP_STATE_USED) {
			if (VTP_Wait(wrk, vtp, VTIM_real() +
			    bo->htc->first_byte_timeout) != 0) {
				bo->htc->doclose = SC_RX_TIMEOUT;
				VSLb(bo->vsl, SLT_FetchError,
				     "Timed out reusing backend connection");
				extrachance = 0;
			}
		}

		if (bo->htc->doclose == SC_NULL) {
			assert(vtp->state == VTP_STATE_USED);
			if (i == 0)
				i = V1F_FetchRespHdr(bo);
			if (i == 0) {
				AN(bo->htc->priv);
				return (0);
			}
		}

		/*
		 * If we recycled a backend connection, there is a finite chance
		 * that the backend closed it before we got the bereq to it.
		 * In that case do a single automatic retry if req.body allows.
		 */
		vbe_dir_finish(d, wrk, bo);
		AZ(bo->htc);
		if (i < 0 || extrachance == 0)
			break;
		if (bo->req != NULL &&
		    bo->req->req_body_status != REQ_BODY_NONE &&
		    bo->req->req_body_status != REQ_BODY_CACHED)
			break;
		VSC_C_main->backend_retry++;
	} while (extrachance--);
	return (-1);
}

static const struct suckaddr * v_matchproto_(vdi_getip_f)
vbe_dir_getip(const struct director *d, struct worker *wrk,
    struct busyobj *bo)
{
	struct vtp *vtp;

	CHECK_OBJ_NOTNULL(d, DIRECTOR_MAGIC);
	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	CHECK_OBJ_NOTNULL(bo, BUSYOBJ_MAGIC);
	CHECK_OBJ_NOTNULL(bo->htc, HTTP_CONN_MAGIC);
	CAST_OBJ_NOTNULL(vtp, bo->htc->priv, VTP_MAGIC);

	return (vtp->addr);
}

/*--------------------------------------------------------------------*/

static enum sess_close
vbe_dir_http1pipe(const struct director *d, struct req *req, struct busyobj *bo)
{
	int i;
	enum sess_close retval;
	struct backend *bp;
	struct v1p_acct v1a;
	struct vtp *vtp;

	CHECK_OBJ_NOTNULL(d, DIRECTOR_MAGIC);
	CHECK_OBJ_NOTNULL(req, REQ_MAGIC);
	CHECK_OBJ_NOTNULL(bo, BUSYOBJ_MAGIC);
	CAST_OBJ_NOTNULL(bp, d->priv, BACKEND_MAGIC);

	memset(&v1a, 0, sizeof v1a);

	/* This is hackish... */
	v1a.req = req->acct.req_hdrbytes;
	req->acct.req_hdrbytes = 0;

	req->res_mode = RES_PIPE;

	vtp = vbe_dir_getfd(req->wrk, bp, bo, 0);

	if (vtp == NULL) {
		retval = SC_TX_ERROR;
	} else {
		i = V1F_SendReq(req->wrk, bo, &v1a.bereq, 1);
		VSLb_ts_req(req, "Pipe", W_TIM_real(req->wrk));
		if (i == 0)
			V1P_Process(req, vtp->fd, &v1a);
		VSLb_ts_req(req, "PipeSess", W_TIM_real(req->wrk));
		bo->htc->doclose = SC_TX_PIPE;
		vbe_dir_finish(d, req->wrk, bo);
		retval = SC_TX_PIPE;
	}
	V1P_Charge(req, &v1a, bp->vsc);
	return (retval);
}

/*--------------------------------------------------------------------*/

static void
vbe_dir_event(const struct director *d, enum vcl_event_e ev)
{
	struct backend *bp;
	struct VSC_vbe *vsc;

	CHECK_OBJ_NOTNULL(d, DIRECTOR_MAGIC);
	CAST_OBJ_NOTNULL(bp, d->priv, BACKEND_MAGIC);

	if (ev == VCL_EVENT_WARM) {
		AZ(bp->vsc);
		bp->vsc = VSC_vbe_New(bp->director->display_name);
		AN(bp->vsc);
	}

	if (bp->probe != NULL && ev == VCL_EVENT_WARM)
		VBP_Control(bp, 1);

	if (bp->probe != NULL && ev == VCL_EVENT_COLD)
		VBP_Control(bp, 0);

	if (ev == VCL_EVENT_COLD) {
		AN(bp->vsc);
		Lck_Lock(&backends_mtx);
		vsc = bp->vsc;
		bp->vsc = NULL;
		Lck_Unlock(&backends_mtx);
		VSC_vbe_Destroy(&vsc);
		AZ(bp->vsc);
	}
}

/*---------------------------------------------------------------------*/

static void v_matchproto_(vdi_destroy_f)
vbe_destroy(const struct director *d)
{
	struct backend *be;

	ASSERT_CLI();
	CAST_OBJ_NOTNULL(be, d->priv, BACKEND_MAGIC);

	if (be->probe != NULL)
		VBP_Remove(be);

	Lck_Lock(&backends_mtx);
	if (be->cooled > 0)
		VTAILQ_REMOVE(&cool_backends, be, list);
	else
		VTAILQ_REMOVE(&backends, be, list);
	VSC_C_main->n_backend--;
	VTP_Rel(&be->tcp_pool);
	Lck_Unlock(&backends_mtx);

#define DA(x)	do { if (be->x != NULL) free(be->x); } while (0)
#define DN(x)	/**/
	VRT_BACKEND_HANDLE();
#undef DA
#undef DN

	AZ(be->vsc);
	Lck_Delete(&be->mtx);
	FREE_OBJ(be);
}

/*--------------------------------------------------------------------*/

static void
vbe_panic(const struct director *d, struct vsb *vsb)
{
	struct backend *bp;

	CHECK_OBJ_NOTNULL(d, DIRECTOR_MAGIC);
	CAST_OBJ_NOTNULL(bp, d->priv, BACKEND_MAGIC);

	VSB_printf(vsb, "display_name = %s,\n", bp->director->display_name);
	if (bp->ipv4_addr != NULL)
		VSB_printf(vsb, "ipv4 = %s,\n", bp->ipv4_addr);
	if (bp->ipv6_addr != NULL)
		VSB_printf(vsb, "ipv6 = %s,\n", bp->ipv6_addr);
	VSB_printf(vsb, "port = %s,\n", bp->port);
	VSB_printf(vsb, "hosthdr = %s,\n", bp->hosthdr);
	VSB_printf(vsb, "health = %s,\n",
	    bp->director->health ? "healthy" : "sick");
	VSB_printf(vsb, "admin_health = %s, changed = %f,\n",
	    VDI_Ahealth(bp->director),
	    bp->director->health_changed);
	VSB_printf(vsb, "n_conn = %u,\n", bp->n_conn);
}

/*--------------------------------------------------------------------
 * Create a new static or dynamic director::backend instance.
 */

struct director *
VRT_new_backend(VRT_CTX, const struct vrt_backend *vrt)
{
	struct backend *be;
	struct director *d;
	struct vcl *vcl;
	const struct vrt_backend_probe *vbp;
	int retval;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	CHECK_OBJ_NOTNULL(vrt, VRT_BACKEND_MAGIC);
	assert(vrt->ipv4_suckaddr != NULL || vrt->ipv6_suckaddr != NULL);

	vcl = ctx->vcl;
	AN(vcl);
	AN(vrt->vcl_name);

	/* Create new backend */
	ALLOC_OBJ(be, BACKEND_MAGIC);
	XXXAN(be);
	Lck_New(&be->mtx, lck_backend);

#define DA(x)	do { if (vrt->x != NULL) REPLACE((be->x), (vrt->x)); } while (0)
#define DN(x)	do { be->x = vrt->x; } while (0)
	VRT_BACKEND_HANDLE();
#undef DA
#undef DN

	d = be->director;
	INIT_OBJ(d, DIRECTOR_MAGIC);
	d->priv = be;
	d->name = "backend";
	d->vcl_name = be->vcl_name;
	d->http1pipe = vbe_dir_http1pipe;
	d->healthy = vbe_dir_healthy;
	d->uptime = vbe_dir_uptime;
	d->gethdrs = vbe_dir_gethdrs;
	d->getip = vbe_dir_getip;
	d->finish = vbe_dir_finish;
	d->event = vbe_dir_event;
	d->panic = vbe_panic;
	d->destroy = vbe_destroy;

	d->health = 1;
	d->health_changed = VTIM_real();
	d->admin_health = VDI_AH_PROBE;

	vbp = vrt->probe;
	if (vbp == NULL)
		vbp = VCL_DefaultProbe(vcl);

	Lck_Lock(&backends_mtx);
	VTAILQ_INSERT_TAIL(&backends, be, list);
	VSC_C_main->n_backend++;
	be->tcp_pool = VTP_Ref(vrt->ipv4_suckaddr, vrt->ipv6_suckaddr,
	    vbe_proto_ident);
	Lck_Unlock(&backends_mtx);

	if (vbp != NULL) {
		VTP_AddRef(be->tcp_pool);
		VBP_Insert(be, vbp, be->tcp_pool);
	}

	retval = VCL_AddDirector(ctx->vcl, d, vrt->vcl_name);

	if (retval == 0)
		return (d);

	VRT_delete_backend(ctx, &d);
	AZ(d);
	return (NULL);
}

/*--------------------------------------------------------------------
 * Delete a dynamic director::backend instance.  Undeleted dynamic and
 * static instances are GC'ed when the VCL is discarded (in cache_vcl.c)
 */

void
VRT_delete_backend(VRT_CTX, struct director **dp)
{
	struct director *d;
	struct backend *be;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	TAKE_OBJ_NOTNULL(d, dp, DIRECTOR_MAGIC);
	CAST_OBJ_NOTNULL(be, d->priv, BACKEND_MAGIC);
	Lck_Lock(&be->mtx);
	be->director->admin_health = VDI_AH_DELETED;
	be->director->health_changed = VTIM_real();
	be->cooled = VTIM_real() + 60.;
	Lck_Unlock(&be->mtx);
	Lck_Lock(&backends_mtx);
	VTAILQ_REMOVE(&backends, be, list);
	VTAILQ_INSERT_TAIL(&cool_backends, be, list);
	Lck_Unlock(&backends_mtx);

	// NB. The backend is still usable for the ongoing transactions,
	// this is why we don't bust the director's magic number.
}

void
VBE_SetHappy(const struct backend *be, uint64_t happy)
{

	CHECK_OBJ_NOTNULL(be, BACKEND_MAGIC);
	Lck_Lock(&backends_mtx);
	if (be->vsc != NULL)
		be->vsc->happy = happy;
	Lck_Unlock(&backends_mtx);
}

/*---------------------------------------------------------------------*/

void
VBE_Poll(void)
{
	struct backend *be, *be2;
	double now = VTIM_real();

	ASSERT_CLI();
	Lck_Lock(&backends_mtx);
	VTAILQ_FOREACH_SAFE(be, &cool_backends, list, be2) {
		CHECK_OBJ_NOTNULL(be, BACKEND_MAGIC);
		if (be->cooled > now)
			break;
		if (be->n_conn > 0)
			continue;
		Lck_Unlock(&backends_mtx);
		VCL_DelDirector(be->director);
		Lck_Lock(&backends_mtx);
	}
	Lck_Unlock(&backends_mtx);
}

/*---------------------------------------------------------------------*/

void
VBE_InitCfg(void)
{

	Lck_New(&backends_mtx, lck_vbe);
}
