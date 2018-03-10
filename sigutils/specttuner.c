/*

  Copyright (C) 2018 Gonzalo José Carracedo Carballal

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation, either version 3 of the
  License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this program.  If not, see
  <http://www.gnu.org/licenses/>

*/

#define SU_LOG_DOMAIN "specttuner"

#include <string.h>
#include <stdlib.h>
#include "sampling.h"
#include "specttuner.h"
#include "log.h"

SUPRIVATE void
su_specttuner_channel_destroy(su_specttuner_channel_t *channel)
{
  if (channel->plan != NULL)
    SU_FFTW(_destroy_plan) (channel->plan);

  if (channel->window != NULL)
    SU_FFTW(_free) (channel->window);

  if (channel->fft != NULL)
    SU_FFTW(_free) (channel->fft);

  free(channel);
}

SUPRIVATE su_specttuner_channel_t *
su_specttuner_channel_new(
    const su_specttuner_t *owner,
    const struct sigutils_specttuner_channel_params *params)
{
  su_specttuner_channel_t *new = NULL;

  SU_TRYCATCH(params->bw > 0 && params->bw < 2 * PI, goto fail);
  SU_TRYCATCH(params->f0 > 0 && params->f0 < 2 * PI, goto fail);

  SU_TRYCATCH(new = calloc(1, sizeof(su_specttuner_channel_t)), goto fail);

  new->params = *params;
  new->index = -1;

  new->decimation = 2 * PI / params->bw;
  new->k = 1. / new->decimation;

  new->center = SU_ROUND(params->f0 / (2 * PI) * owner->params.window_size);
  new->size   = SU_CEIL(new->k * owner->params.window_size);

  new->k /= new->size;
  SU_TRYCATCH(new->size > 0, goto fail);

  new->halfsz = new->size >> 1;
  new->offset = new->size >> 2;

  new->width  = new->size;
  new->halfw  = new->width >> 1;

  /* FFT initialization */
  SU_TRYCATCH(
      new->window = SU_FFTW(_malloc)(new->size * sizeof(SU_FFTW(_complex))),
      goto fail);

  SU_TRYCATCH(
      new->fft = SU_FFTW(_malloc)(new->size * sizeof(SU_FFTW(_complex))),
      goto fail);

  memset(new->fft, 0, new->size * sizeof(SU_FFTW(_complex)));

  SU_TRYCATCH(
      new->plan = SU_FFTW(_plan_dft_1d)(
          new->size,
          new->fft,
          new->window,
          FFTW_BACKWARD,
          FFTW_ESTIMATE),
      goto fail);

  return new;

fail:
  if (new != NULL)
    su_specttuner_channel_destroy(new);

  return NULL;
}

void
su_specttuner_destroy(su_specttuner_t *st)
{
  unsigned int i;

  for (i = 0; i < st->channel_count; ++i)
    if (st->channel_list[i] != NULL)
      su_specttuner_close_channel(st, st->channel_list[i]);

  if (st->channel_list != NULL)
    free(st->channel_list);

  if (st->plans[SU_SPECTTUNER_STATE_EVEN] != NULL)
    SU_FFTW(_destroy_plan) (st->plans[SU_SPECTTUNER_STATE_EVEN]);

  if (st->plans[SU_SPECTTUNER_STATE_ODD] != NULL)
    SU_FFTW(_destroy_plan) (st->plans[SU_SPECTTUNER_STATE_ODD]);

  if (st->fft != NULL)
    SU_FFTW(_free) (st->fft);

  if (st->window != NULL)
    SU_FFTW(_free) (st->window);

  free(st);
}

su_specttuner_t *
su_specttuner_new(const struct sigutils_specttuner_params *params)
{
  su_specttuner_t *new = NULL;

  SU_TRYCATCH((params->window_size & 1) == 0, goto fail);

  SU_TRYCATCH(new = calloc(1, sizeof(su_specttuner_t)), goto fail);

  new->params = *params;
  new->half_size = params->window_size >> 1;
  new->full_size = 3 * params->window_size;

  /* Window is 3/2 the FFT size */
  SU_TRYCATCH(
      new->window = SU_FFTW(_malloc(
          new->full_size * sizeof(SU_FFTW(_complex)))),
      goto fail);

  /* FFT is the size provided by params */
  SU_TRYCATCH(
      new->fft = SU_FFTW(_malloc(
          params->window_size * sizeof(SU_FFTW(_complex)))),
      goto fail);

  /* Even plan starts at the beginning of the window */
  SU_TRYCATCH(
      new->plans[SU_SPECTTUNER_STATE_EVEN] = SU_FFTW(_plan_dft_1d)(
          params->window_size,
          new->window,
          new->fft,
          FFTW_FORWARD,
          FFTW_ESTIMATE),
      goto fail);

  /* Odd plan stars at window_size / 2 */
  SU_TRYCATCH(
      new->plans[SU_SPECTTUNER_STATE_ODD] = SU_FFTW(_plan_dft_1d)(
          params->window_size,
          new->window + new->half_size,
          new->fft,
          FFTW_FORWARD,
          FFTW_ESTIMATE),
      goto fail);

  return new;

fail:
  if (new != NULL)
    su_specttuner_destroy(new);

  return NULL;
}

SUINLINE SUSCOUNT
__su_specttuner_feed_bulk(
    su_specttuner_t *st,
    const SUCOMPLEX *buf,
    SUSCOUNT size)
{
  SUSDIFF halfsz;
  SUSDIFF p;

  if (size + st->p > st->params.window_size)
    size = st->params.window_size - st->p;

  switch (st->state)
  {
    case SU_SPECTTUNER_STATE_EVEN:
      /* Just copy at the beginning */
      memcpy(st->window + st->p, buf, size * sizeof(SUCOMPLEX));
      break;

    case SU_SPECTTUNER_STATE_ODD:
      /* Copy to the second third */
      memcpy(st->window + st->p + st->half_size, buf, size * sizeof(SUCOMPLEX));

      /* Did this copy populate the last third? */
      if (st->p + size > st->half_size) {
        halfsz = st->p + size - st->half_size;
        p = st->p > st->half_size ? st->p : st->half_size;

        /* Don't take into account data already written */
        halfsz -= p - st->half_size;

        /* Copy to the first third */
        if (halfsz > 0)
          memcpy(
              st->window + p - st->half_size,
              st->window + p + st->half_size,
              halfsz * sizeof(SUCOMPLEX));
      }
  }

  st->p += size;

  if (st->p == st->params.window_size) {
    st->p = st->half_size;

    /* Compute FFT */
    SU_FFTW(_execute) (st->plans[st->state]);

    /* Toggle state */
    st->state = !st->state;
    st->ready = SU_TRUE;
  }

  return size;
}

SUINLINE SUBOOL
__su_specttuner_feed_channel(
    su_specttuner_t *st,
    const su_specttuner_channel_t *channel)
{
  int p;
  int len;
  int window_size = st->params.window_size;
  unsigned int i;
  SUCOMPLEX k_phase;

  p = channel->center;

  /***************************** Upper sideband ******************************/
  len = channel->halfw;
  if (p + len > window_size) /* Test for rollover */
    len = window_size - p;

  /* Copy to the end */
  memcpy(
      channel->fft,
      st->fft + p,
      len * sizeof(SUCOMPLEX));

  /* Copy remaining part */
  if (len < channel->halfw)
    memcpy(
        channel->fft + len,
        st->fft,
        (channel->halfw - len) * sizeof(SUCOMPLEX));

  /***************************** Lower sideband ******************************/
  len = channel->halfw;
  if (p < len) /* Roll over */
    len = p; /* Can copy up to p bytes */


  /* Copy higher frequencies */
  memcpy(
      channel->fft + channel->size - len,
      st->fft + p - len,
      len * sizeof(SUCOMPLEX));

  /* Copy remaining part */
  if (len < channel->halfw)
    memcpy(
        channel->fft + channel->size - channel->halfw,
        st->fft + window_size - (channel->halfw - len),
        (channel->halfw - len) * sizeof(SUCOMPLEX));

  /****************** Apply phase correction and scaling *********************/
  for (i = 0; i < channel->halfw; ++i)
    channel->fft[i] *= channel->k;

  for (i = channel->size - channel->halfw; i < channel->size; ++i)
    channel->fft[i] *= channel->k;

  /************************* Back to time domain******************************/
  SU_FFTW(_execute) (channel->plan);

  /************************** Call user callback *****************************/
  return (channel->params.on_data) (
      channel,
      channel->params.private,
      channel->window + channel->offset,
      channel->halfsz);
}

SUBOOL
su_specttuner_feed_bulk(
    su_specttuner_t *st,
    const SUCOMPLEX *buf,
    SUSCOUNT size)
{
  SUSCOUNT got;
  unsigned int i;
  SUBOOL ok = SU_TRUE;

  while (size > 0) {
    got = __su_specttuner_feed_bulk(st, buf, size);

    /* Buffer full, feed channels */
    if (st->ready) {
      st->ready = SU_FALSE;

      for (i = 0; i < st->channel_count; ++i)
        if (st->channel_list[i] != NULL)
          ok = __su_specttuner_feed_channel(st, st->channel_list[i]) && ok;
    }

    buf += got;
    size -= got;
  }

  return ok;
}

su_specttuner_channel_t *
su_specttuner_open_channel(
    su_specttuner_t *st,
    const struct sigutils_specttuner_channel_params *params)
{
  su_specttuner_channel_t *new = NULL;
  int index;

  SU_TRYCATCH(new = su_specttuner_channel_new(st, params), goto fail);

  SU_TRYCATCH(
      (index = PTR_LIST_APPEND_CHECK(st->channel, new)) != -1,
      goto fail);

  new->index = index;

  return new;

fail:
  if (new != NULL)
    su_specttuner_channel_destroy(new);

  return NULL;
}

SUBOOL
su_specttuner_close_channel(
    su_specttuner_t *st,
    su_specttuner_channel_t *channel)
{
  SU_TRYCATCH(channel->index >= 0, return SU_FALSE);

  SU_TRYCATCH(channel->index < st->channel_count, return SU_FALSE);

  SU_TRYCATCH(st->channel_list[channel->index] == channel, return SU_FALSE);

  st->channel_list[channel->index] = NULL;

  su_specttuner_channel_destroy(channel);

  return SU_TRUE;
}
