/** OGG Vorbis input/output.
Copyright (c) 2015 Simon Zolin */

#include <fmedia.h>

#include <FF/audio/ogg.h>
#include <FF/audio/pcm.h>
#include <FF/data/mmtag.h>
#include <FF/array.h>
#include <FF/path.h>
#include <FFOS/error.h>
#include <FFOS/random.h>
#include <FFOS/timer.h>


static const fmed_core *core;
static const fmed_queue *qu;

typedef struct fmed_ogg {
	ffogg og;
	uint state;
} fmed_ogg;

typedef struct ogg_out {
	ffogg_enc og;
	uint state;
	uint hdr_done :1;
} ogg_out;

static struct ogg_in_conf_t {
	byte seekable;
} ogg_in_conf;

static struct ogg_out_conf_t {
	ushort min_tag_size;
	ushort page_size;
	float qual;
} ogg_out_conf;


//FMEDIA MODULE
static const void* ogg_iface(const char *name);
static int ogg_sig(uint signo);
static void ogg_destroy(void);
static const fmed_mod fmed_ogg_mod = {
	&ogg_iface, &ogg_sig, &ogg_destroy
};

//DECODE
static void* ogg_open(fmed_filt *d);
static void ogg_close(void *ctx);
static int ogg_decode(void *ctx, fmed_filt *d);
static int ogg_conf(ffpars_ctx *ctx);
static const fmed_filter fmed_ogg_input = {
	&ogg_open, &ogg_decode, &ogg_close, &ogg_conf
};

static const ffpars_arg ogg_in_conf_args[] = {
	{ "seekable",  FFPARS_TBOOL | FFPARS_F8BIT,  FFPARS_DSTOFF(struct ogg_in_conf_t, seekable) }
};

//ENCODE
static void* ogg_out_open(fmed_filt *d);
static void ogg_out_close(void *ctx);
static int ogg_out_encode(void *ctx, fmed_filt *d);
static int ogg_out_config(ffpars_ctx *ctx);
static const fmed_filter fmed_ogg_output = {
	&ogg_out_open, &ogg_out_encode, &ogg_out_close, &ogg_out_config
};

static const ffpars_arg ogg_out_conf_args[] = {
	{ "min_tag_size",  FFPARS_TINT | FFPARS_F16BIT,  FFPARS_DSTOFF(struct ogg_out_conf_t, min_tag_size) },
	{ "page_size",  FFPARS_TSIZE | FFPARS_F16BIT | FFPARS_FNOTZERO,  FFPARS_DSTOFF(struct ogg_out_conf_t, page_size) },
	{ "quality",  FFPARS_TFLOAT | FFPARS_FSIGN,  FFPARS_DSTOFF(struct ogg_out_conf_t, qual) }
};


FF_EXP const fmed_mod* fmed_getmod(const fmed_core *_core)
{
	fftime t;
	fftime_now(&t);
	ffrnd_seed(t.s);

	ffmem_init();
	core = _core;
	return &fmed_ogg_mod;
}


static const void* ogg_iface(const char *name)
{
	if (!ffsz_cmp(name, "decode")) {
		ogg_in_conf.seekable = 1;
		return &fmed_ogg_input;

	} else if (!ffsz_cmp(name, "encode")) {
		ogg_out_conf.min_tag_size = 1000;
		ogg_out_conf.page_size = 8 * 1024;
		ogg_out_conf.qual = 5.0;
		return &fmed_ogg_output;
	}
	return NULL;
}

static int ogg_sig(uint signo)
{
	switch (signo) {
	case FMED_OPEN:
		qu = core->getmod("#queue.queue");
		break;
	}
	return 0;
}

static void ogg_destroy(void)
{
}


static int ogg_conf(ffpars_ctx *ctx)
{
	ffpars_setargs(ctx, &ogg_in_conf, ogg_in_conf_args, FFCNT(ogg_in_conf_args));
	return 0;
}

static void* ogg_open(fmed_filt *d)
{
	fmed_ogg *o = ffmem_tcalloc1(fmed_ogg);
	if (o == NULL)
		return NULL;
	ffogg_init(&o->og);

	if ((int64)d->input.size != FMED_NULL)
		o->og.total_size = d->input.size;

	if (ogg_in_conf.seekable)
		o->og.seekable = 1;
	return o;
}

static void ogg_close(void *ctx)
{
	fmed_ogg *o = ctx;
	ffogg_close(&o->og);
	ffmem_free(o);
}

/** Check if the output is also OGG.
If so, don't decode audio but just pass OGG pages as-is. */
static int ogg_asis_check(fmed_ogg *o, fmed_filt *d)
{
	if (1 != fmed_getval("stream_copy"))
		return 0;

	d->track->setvalstr(d->trk, "data_asis", "ogg");
	int64 samples = -1;
	if ((int64)d->audio.seek != FMED_NULL) {
		samples = ffpcm_samples(d->audio.seek, ffogg_rate(&o->og));
		d->audio.seek = FMED_NULL;
	}
	ffogg_set_asis(&o->og, samples);
	return 1;
}

static int ogg_readasis(fmed_ogg *o, fmed_filt *d)
{
	int r;

	for (;;) {
	r = ffogg_readasis(&o->og);

	switch (r) {
	case FFOGG_RPAGE:
		goto data;

	case FFOGG_RDONE:
		d->outlen = 0;
		return FMED_RLASTOUT;

	case FFOGG_RMORE:
		if (d->flags & FMED_FLAST) {
			dbglog(core, d->trk, "ogg", "no eos page");
			d->outlen = 0;
			return FMED_RLASTOUT;
		}
		return FMED_RMORE;

	case FFOGG_RSEEK:
		d->input.seek = o->og.off;
		return FMED_RMORE;

	case FFOGG_RWARN:
		warnlog(core, d->trk, "ogg", "near sample %U: ffogg_decode(): %s"
			, ffogg_cursample(&o->og), ffogg_errstr(o->og.err));
		break;

	default:
		errlog(core, d->trk, "ogg", "ffogg_decode(): %s", ffogg_errstr(o->og.err));
		return FMED_RERR;
	}
	}

data:
	d->audio.pos = ffogg_cursample(&o->og);
	d->data = o->og.data;
	d->datalen = o->og.datalen;
	ffogg_pagedata(&o->og, &d->out, &d->outlen);
	return FMED_RDATA;
}

static int ogg_decode(void *ctx, fmed_filt *d)
{
	enum { I_HDR, I_DATA0, I_DATA, I_ASIS };
	fmed_ogg *o = ctx;
	int r;

	if (d->flags & FMED_FSTOP) {
		d->outlen = 0;
		return FMED_RLASTOUT;
	}

	o->og.data = d->data;
	o->og.datalen = d->datalen;

again:
	switch (o->state) {
	case I_ASIS:
		return ogg_readasis(o, d);

	case I_HDR:
		break;

	case I_DATA0:
		if (d->input_info)
			return FMED_ROK;
		if (ogg_asis_check(o, d)) {
			o->state = I_ASIS;
			goto again;
		}
		o->state = I_DATA;
		// break

	case I_DATA:
		if ((int64)d->audio.seek != FMED_NULL) {
			ffogg_seek(&o->og, ffpcm_samples(d->audio.seek, ffogg_rate(&o->og)));
			d->audio.seek = FMED_NULL;
		}
		break;
	}

	for (;;) {
		r = ffogg_decode(&o->og);
		switch (r) {
		case FFOGG_RMORE:
			if (d->flags & FMED_FLAST) {
				dbglog(core, d->trk, "ogg", "no eos page");
				d->outlen = 0;
				return FMED_RLASTOUT;
			}
			return FMED_RMORE;

		case FFOGG_RPAGE:
			d->audio.pos = ffogg_cursample(&o->og);
			d->data = o->og.data;
			d->datalen = o->og.datalen;
			ffogg_pagedata(&o->og, &d->out, &d->outlen);
			return FMED_RDATA;

		case FFOGG_RDATA:
			goto data;

		case FFOGG_RDONE:
			d->outlen = 0;
			return FMED_RLASTOUT;

		case FFOGG_RHDR:
			d->track->setvalstr(d->trk, "pcm_decoder", "Vorbis");
			d->audio.fmt.format = FFPCM_FLOAT;
			d->audio.fmt.channels = ffogg_channels(&o->og);
			d->audio.fmt.sample_rate = ffogg_rate(&o->og);
			d->audio.fmt.ileaved = 0;
			break;

		case FFOGG_RTAG: {
			const ffvorbtag *vtag = &o->og.vtag;
			dbglog(core, d->trk, "ogg", "%S: %S", &vtag->name, &vtag->val);
			ffstr name = vtag->name;
			if (vtag->tag != 0)
				ffstr_setz(&name, ffmmtag_str[vtag->tag]);
			qu->meta_set((void*)fmed_getval("queue_item"), name.ptr, name.len, vtag->val.ptr, vtag->val.len, FMED_QUE_TMETA);
			}
			break;

		case FFOGG_RHDRFIN:
			if (!ogg_in_conf.seekable) {
				o->state = I_DATA0;
				goto again;
			}
			break;

		case FFOGG_RINFO:
			d->audio.total = o->og.total_samples;
			fmed_setval("bitrate", ffogg_bitrate(&o->og));
			o->state = I_DATA0;
			goto again;

		case FFOGG_RSEEK:
			d->input.seek = o->og.off;
			return FMED_RMORE;

		case FFOGG_RWARN:
			warnlog(core, d->trk, "ogg", "near sample %U: ffogg_decode(): %s"
				, ffogg_cursample(&o->og), ffogg_errstr(o->og.err));
			break;

		default:
			errlog(core, d->trk, "ogg", "ffogg_decode(): %s", ffogg_errstr(o->og.err));
			return FMED_RERR;
		}
	}

data:
	dbglog(core, d->trk, "ogg", "decoded %u PCM samples, page: %u, granule pos: %U"
		, o->og.nsamples, ffogg_pageno(&o->og), ffogg_granulepos(&o->og));
	d->audio.pos = ffogg_cursample(&o->og);

	d->data = o->og.data;
	d->datalen = o->og.datalen;
	d->outni = (void**)o->og.pcm;
	d->outlen = o->og.pcmlen;
	return FMED_RDATA;
}


static int ogg_out_config(ffpars_ctx *ctx)
{
	ffpars_setargs(ctx, &ogg_out_conf, ogg_out_conf_args, FFCNT(ogg_out_conf_args));
	return 0;
}

static void* ogg_out_open(fmed_filt *d)
{
	const char *copyfmt;
	if (FMED_PNULL != (copyfmt = d->track->getvalstr(d->trk, "data_asis"))) {
		if (ffsz_cmp(copyfmt, "ogg")) {
			errlog(core, d->trk, NULL, "unsupported input data format: %s", copyfmt);
			return NULL;
		}
		return FMED_FILT_SKIP;
	}

	ogg_out *o = ffmem_tcalloc1(ogg_out);
	if (o == NULL)
		return NULL;

	ffogg_enc_init(&o->og);
	o->og.min_tagsize = ogg_out_conf.min_tag_size;
	return o;
}

static void ogg_out_close(void *ctx)
{
	ogg_out *o = ctx;
	ffogg_enc_close(&o->og);
	ffmem_free(o);
}

static int ogg_out_addmeta(ogg_out *o, fmed_filt *d)
{
	uint i;
	ffstr name, *val;
	void *qent;

	if (FMED_PNULL == (qent = (void*)fmed_getval("queue_item")))
		return 0;

	for (i = 0;  NULL != (val = qu->meta(qent, i, &name, FMED_QUE_UNIQ));  i++) {
		if (val == FMED_QUE_SKIP
			|| ffstr_eqcz(&name, "vendor"))
			continue;
		if (0 != ffogg_addtag(&o->og, name.ptr, val->ptr, val->len))
			warnlog(core, d->trk, "ogg", "can't add tag: %S", &name);
	}
	return 0;
}

static int ogg_out_encode(void *ctx, fmed_filt *d)
{
	enum { I_CONF, I_CREAT, I_INPUT, I_ENCODE };
	ogg_out *o = ctx;
	int r, qual;
	ffpcm fmt;

	switch (o->state) {
	case I_CONF:
		if (d->audio.fmt.format != FFPCM_FLOAT || d->audio.fmt.ileaved) {
			if (d->audio.fmt.format != FFPCM_FLOAT)
				fmed_setval("conv_pcm_format", FFPCM_FLOAT);

			if (d->audio.fmt.ileaved)
				fmed_setval("conv_pcm_ileaved", 0);

			o->state = I_CREAT;
			return FMED_RMORE;
		}
		//break;

	case I_CREAT:
		ffpcm_fmtcopy(&fmt, &d->audio.fmt);
		if (fmt.format != FFPCM_FLOAT || d->audio.fmt.ileaved) {
			errlog(core, d->trk, "ogg", "input format must be PCM-float non-interleaved");
			return FMED_RERR;
		}

		ogg_out_addmeta(o, d);

		qual = (int)fmed_getval("ogg-quality");
		if (qual == FMED_NULL)
			qual = ogg_out_conf.qual * 10;

		o->og.max_pagesize = ogg_out_conf.page_size;
		if (0 != (r = ffogg_create(&o->og, &fmt, qual, ffrnd_get()))) {
			errlog(core, d->trk, "ogg", "ffogg_create() failed: %s", ffogg_errstr(r));
			return FMED_RERR;
		}
		//o->state = I_INPUT;
		//break;

	case I_INPUT:
		o->og.pcm = (const float**)d->data;
		o->og.pcmlen = d->datalen;
		if (d->flags & FMED_FLAST)
			o->og.fin = 1;
		o->state = I_ENCODE;
		//break;

	case I_ENCODE:
		break;
	}

	for (;;) {
		r = ffogg_encode(&o->og);
		switch (r) {

		case FFOGG_RDATA:
			if (!o->hdr_done) {
				o->hdr_done = 1;
				if ((int64)d->audio.total != FMED_NULL)
					d->output.size = ffogg_enc_size(&o->og, d->audio.total - d->audio.pos);
			}
			goto data;

		case FFOGG_RDONE:
			d->outlen = 0;
			return FMED_RLASTOUT;

		case FFOGG_RMORE:
			o->state = I_INPUT;
			return FMED_RMORE;

		default:
			errlog(core, d->trk, "ogg", "ffogg_encode() failed: %s", ffogg_errstr(o->og.err));
			return FMED_RERR;
		}
	}

data:
	d->out = o->og.data;
	d->outlen = o->og.datalen;

	dbglog(core, d->trk, "ogg", "output: %L bytes, page: %u"
		, (size_t)d->outlen, o->og.page.number);

	return FMED_RDATA;
}
