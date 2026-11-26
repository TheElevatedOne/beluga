// SPDX-License-Identifier: GPL-2.0-or-later
#include "mediainfo.hpp"

#include "util.hpp"

#ifdef BELUGA_HAVE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/log.h>
#include <libavutil/pixdesc.h>
}
#include <algorithm>
#include <cmath>
#include <mutex>
#endif

namespace beluga::mediainfo {

#ifdef BELUGA_HAVE_FFMPEG

bool available() { return true; }

namespace {

// The tags people look for, in this order, with their labels; others follow under their own key.
struct KnownTag {
    const char *key, *label;
};
const KnownTag kTags[] = {
    {"title", N_("Title:")},         {"artist", N_("Artist:")},     {"album_artist", N_("Album artist:")},
    {"album", N_("Album:")},         {"track", N_("Track:")},       {"disc", N_("Disc:")},
    {"date", N_("Date:")},           {"genre", N_("Genre:")},       {"composer", N_("Composer:")},
    {"performer", N_("Performer:")}, {"publisher", N_("Publisher:")}, {"copyright", N_("Copyright:")},
    {"comment", N_("Comment:")},     {"description", N_("Description:")}, {"language", N_("Language:")},
};

std::string clip(const char *v)
{
    std::string s = v ? v : "";
    for (auto &c : s)
        if (c == '\r' || c == '\n' || c == '\t') c = ' ';
    if (s.size() > 300) {
        size_t cut = 300;
        while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80) --cut;   // not inside a UTF-8 character
        s = s.substr(0, cut) + "…";
    }
    return s;
}

std::string bitrate(int64_t bps)
{
    if (bps <= 0) return {};
    if (bps >= 1000000) return printf_str(_("%.1f Mbit/s"), bps / 1e6);
    return printf_str(_("%lld kbit/s"), (long long)((bps + 500) / 1000));
}

std::string codec_name(AVCodecID id)
{
    const AVCodecDescriptor *d = avcodec_descriptor_get(id);
    if (!d) return avcodec_get_name(id);
    return d->long_name && *d->long_name ? printf_str("%s (%s)", d->long_name, d->name) : std::string(d->name);
}

void add_tags(const AVDictionary *meta, std::vector<std::pair<std::string, std::string>> &rows)
{
    if (!meta) return;
    for (const KnownTag &k : kTags)
        if (const AVDictionaryEntry *e = av_dict_get(meta, k.key, nullptr, 0))
            if (e->value && *e->value) rows.emplace_back(_(k.label), clip(e->value));
    const AVDictionaryEntry *e = nullptr;
    while ((e = av_dict_get(meta, "", e, AV_DICT_IGNORE_SUFFIX))) {
        if (!e->value || !*e->value) continue;
        std::string key = e->key;
        std::string lower = key;
        for (auto &c : lower) c = g_ascii_tolower(c);
        bool known = false;
        for (const KnownTag &k : kTags)
            if (lower == k.key) known = true;
        // Technical keys that the stream rows already show, or that say nothing to people.
        if (known || lower == "encoder" || lower == "major_brand" || lower == "minor_version" || lower == "compatible_brands"
            || lower == "handler_name" || lower == "vendor_id" || lower == "creation_time" || starts_with(lower, "itunsmpb")
            || starts_with(lower, "itunnorm") || starts_with(lower, "replaygain_") || starts_with(lower, "musicbrainz")
            || starts_with(lower, "acoustid") || lower == "duration")
            continue;
        if (!key.empty()) key[0] = g_ascii_toupper(key[0]);
        std::replace(key.begin(), key.end(), '_', ' ');
        rows.emplace_back(key + ":", clip(e->value));
    }
}

} // namespace

std::vector<std::pair<std::string, std::string>> probe(const std::string &path)
{
    static std::once_flag quiet;
    std::call_once(quiet, [] { av_log_set_level(AV_LOG_QUIET); });
    std::vector<std::pair<std::string, std::string>> rows;
    AVFormatContext *fc = nullptr;
    if (avformat_open_input(&fc, path.c_str(), nullptr, nullptr) < 0) return rows;
    if (avformat_find_stream_info(fc, nullptr) < 0) {
        avformat_close_input(&fc);
        return rows;
    }
    if (fc->duration > 0) rows.emplace_back(_("Duration:"), format_duration(fc->duration / (double)AV_TIME_BASE));
    if (fc->iformat && fc->iformat->long_name) rows.emplace_back(_("Format:"), fc->iformat->long_name);
    if (std::string b = bitrate(fc->bit_rate); !b.empty()) rows.emplace_back(_("Bit rate:"), b);
    add_tags(fc->metadata, rows);

    int videoN = 0, audioN = 0, subs = 0;
    std::vector<std::string> subLangs;
    for (unsigned i = 0; i < fc->nb_streams; ++i) {
        const AVStream *st = fc->streams[i];
        const AVCodecParameters *p = st->codecpar;
        // Cover art is a video stream of one picture; it isn't a video.
        if (st->disposition & AV_DISPOSITION_ATTACHED_PIC) {
            if (p->width > 0) rows.emplace_back(_("Cover:"), printf_str("%d × %d, %s", p->width, p->height, avcodec_get_name(p->codec_id)));
            continue;
        }
        if (p->codec_type == AVMEDIA_TYPE_VIDEO) {
            ++videoN;
            std::string v = codec_name(p->codec_id);
            if (p->width > 0) v += printf_str(", %d × %d", p->width, p->height);
            AVRational fr = st->avg_frame_rate.num ? st->avg_frame_rate : st->r_frame_rate;
            if (fr.num > 0 && fr.den > 0) {
                double fps = av_q2d(fr);
                v += std::fabs(fps - std::round(fps)) < 0.005 ? printf_str(_(", %.0f fps"), fps) : printf_str(_(", %.3f fps"), fps);
            }
            if (const char *pf = av_get_pix_fmt_name((AVPixelFormat)p->format)) v += std::string(", ") + pf;
            if (std::string b = bitrate(p->bit_rate); !b.empty()) v += ", " + b;
            rows.emplace_back(videoN == 1 ? _("Video:") : printf_str(_("Video %d:"), videoN), v);
        } else if (p->codec_type == AVMEDIA_TYPE_AUDIO) {
            ++audioN;
            std::string a = codec_name(p->codec_id);
            if (p->sample_rate > 0) a += printf_str(_(", %d Hz"), p->sample_rate);
            if (int ch = p->ch_layout.nb_channels; ch > 0) {
                char layout[128] = "";
                if (p->ch_layout.order != AV_CHANNEL_ORDER_UNSPEC) av_channel_layout_describe(&p->ch_layout, layout, sizeof layout);
                if (*layout) a += std::string(", ") + layout;
                else if (ch == 1) a += std::string(", ") + _("mono");
                else if (ch == 2) a += std::string(", ") + _("stereo");
                else a += ", " + printf_str(g_dngettext(GETTEXT_PACKAGE, "%d channel", "%d channels", (gulong)ch), ch);
            }
            // Lossless codecs say their bit depth; for PCM it follows from the codec.
            int bits = p->bits_per_raw_sample ? p->bits_per_raw_sample : av_get_bits_per_sample(p->codec_id);
            if (bits > 0) a += printf_str(_(", %d bit"), bits);
            if (std::string b = bitrate(p->bit_rate); !b.empty()) a += ", " + b;
            if (const AVDictionaryEntry *l = av_dict_get(st->metadata, "language", nullptr, 0))
                if (l->value && *l->value && strcmp(l->value, "und") != 0) a += std::string(" [") + l->value + "]";
            rows.emplace_back(audioN == 1 ? _("Audio:") : printf_str(_("Audio %d:"), audioN), a);
        } else if (p->codec_type == AVMEDIA_TYPE_SUBTITLE) {
            ++subs;
            if (const AVDictionaryEntry *l = av_dict_get(st->metadata, "language", nullptr, 0))
                if (l->value && *l->value && std::find(subLangs.begin(), subLangs.end(), l->value) == subLangs.end()) subLangs.push_back(l->value);
        }
        // Tags of the only audio stream (Ogg and FLAC-in-something keep them there).
        if (fc->nb_streams == 1 && !av_dict_count(fc->metadata)) add_tags(st->metadata, rows);
    }
    if (subs) {
        std::string s = printf_str("%d", subs);
        if (!subLangs.empty()) s += " (" + join(subLangs, ", ") + ")";
        rows.emplace_back(_("Subtitles:"), s);
    }
    if (fc->nb_chapters > 0) rows.emplace_back(_("Chapters:"), printf_str("%u", fc->nb_chapters));
    avformat_close_input(&fc);
    return rows;
}

#else

bool available() { return false; }
std::vector<std::pair<std::string, std::string>> probe(const std::string &) { return {}; }

#endif

} // namespace beluga::mediainfo
