// Haul's media detector: decides, from a real HTTP response, whether it is a downloadable media
// file, a stream manifest, or neither. No browser APIs here, so it can be tested on its own.
(() => {
  const FORMATS = {
    mp4: "video", mkv: "video", webm: "video", mov: "video", avi: "video", flv: "video", ts: "video",
    m4v: "video", mpeg: "video", mpg: "video", "3gp": "video", wmv: "video", ogv: "video",
    mp3: "audio", wav: "audio", flac: "audio", aac: "audio", m4a: "audio", ogg: "audio", oga: "audio",
    opus: "audio", wma: "audio",
  };

  const MIME_FORMATS = {
    "video/mp4": "mp4", "application/mp4": "mp4", "video/x-m4v": "m4v", "video/webm": "webm",
    "video/x-matroska": "mkv", "video/matroska": "mkv", "application/x-matroska": "mkv",
    "video/quicktime": "mov", "video/x-msvideo": "avi", "video/avi": "avi", "video/msvideo": "avi",
    "video/x-flv": "flv", "application/x-flv": "flv", "video/mp2t": "ts", "video/mpeg": "mpeg",
    "video/3gpp": "3gp", "video/3gpp2": "3gp", "video/x-ms-wmv": "wmv", "video/ogg": "ogv",
    "audio/mpeg": "mp3", "audio/mp3": "mp3", "audio/wav": "wav", "audio/x-wav": "wav", "audio/wave": "wav",
    "audio/vnd.wave": "wav", "audio/flac": "flac", "audio/x-flac": "flac", "audio/aac": "aac",
    "audio/x-aac": "aac", "audio/aacp": "aac", "audio/mp4": "m4a", "audio/x-m4a": "m4a", "audio/m4a": "m4a",
    "audio/ogg": "ogg", "application/ogg": "ogg", "audio/opus": "opus", "audio/x-ms-wma": "wma",
  };

  // Types that say nothing about the content: the name or the player has to decide.
  const GENERIC_MIME = new Set(["", "application/octet-stream", "binary/octet-stream", "application/force-download",
    "application/download", "application/x-download", "application/binary", "application/unknown", "unknown/unknown"]);
  const MANIFEST_MIME = /^(application\/(vnd\.apple\.mpegurl|x-mpegurl|dash\+xml)|audio\/(mpegurl|x-mpegurl))$/;
  const MANIFEST_NAME = /\.(m3u8|m3u|mpd)$/i;

  // Never media, whatever the URL claims.
  const NOT_MEDIA_MIME = /^(text\/|image\/|font\/|application\/(json|javascript|x-javascript|xml|xhtml\+xml|pdf|zip|wasm|manifest\+json))/;

  // Pieces of a stream, never whole files.
  const PIECE_NAME = /\.(m4s|cmfv|cmfa|m4f|init)$/i;
  const NUMBERED_PIECE = /(^|[\/_.-])(seg|segment|chunk|frag|fragment|part|sq)[-_.]?\d+/i;
  const RANGE_QUERY = /^(range|bytes|byterange|bytestart|byteend)$/i;

  // Query values that change between requests for the same file (signatures, expiry times, ...).
  const VOLATILE_QUERY = /^(token|auth|sig|signature|hash|hmac|expires?|exp|e|st|ts|t|policy|key-pair-id|x-amz-.*|x-goog-.*|se|sp|sv|sr|skn|hdnts|hdntl|nonce|cache|cb|_|v)$/i;

  const EXCLUDED_HOSTS = /(^|\.)(youtube\.com|youtube-nocookie\.com|youtu\.be|googlevideo\.com|ytimg\.com)$/i;

  const MIN_PLAYER_SIZE = 100 * 1024;       // element playback; smaller is interface sound
  const MIN_BACKGROUND_SIZE = 1024 * 1024;  // fetched by script; smaller is almost always a piece

  function header(headers, name) {
    const found = (headers || []).find((h) => h.name.toLowerCase() === name);
    return found && found.value ? found.value : "";
  }

  function extensionOf(name) {
    const match = /\.([a-z0-9]{1,5})$/i.exec(name || "");
    return match ? match[1].toLowerCase() : "";
  }

  function decode(text) {
    try {
      return decodeURIComponent(text);
    } catch {
      return text;
    }
  }

  function dispositionName(value) {
    let match = /filename\*\s*=\s*(?:[\w-]+'[^']*')?"?([^";]+)/i.exec(value);
    if (!match) match = /filename\s*=\s*"?([^";]+)/i.exec(value);
    return match ? decode(match[1].trim()).split(/[\\/]/).pop() : "";
  }

  function urlName(url) {
    return decode(url.pathname.split("/").pop() || "");
  }

  // The same media under different signed links gets the same key.
  function mediaKey(address) {
    let url;
    try {
      url = new URL(address);
    } catch {
      return address;
    }
    const kept = [...url.searchParams.entries()].filter(([name]) => !VOLATILE_QUERY.test(name)).sort();
    const query = kept.map(([name, value]) => `${name}=${value}`).join("&");
    return `${url.host}${url.pathname}${query ? `?${query}` : ""}`;
  }

  function isMediaLink(address) {
    try {
      const format = extensionOf(new URL(address).pathname);
      return format !== "ts" && Object.prototype.hasOwnProperty.call(FORMATS, format);
    } catch {
      return false;
    }
  }

  // Adds the right extension to a name that lacks one, e.g. "stream" or "download.php".
  function fileNameFor(name, format, host) {
    const base = name || host || "media";
    const extension = extensionOf(base);
    if (!format || Object.prototype.hasOwnProperty.call(FORMATS, extension)) return base;
    return `${base}.${format}`;
  }

  function responseSize(headers) {
    const total = /\/(\d+)\s*$/.exec(header(headers, "content-range"));
    if (total) return Number(total[1]);
    if (header(headers, "content-encoding")) return 0;
    return Number(header(headers, "content-length")) || 0;
  }

  // details: { url, statusCode, type, responseHeaders }, as webRequest reports them.
  // Returns null, { stream: true, ... } or a media description.
  function classifyResponse(details) {
    if (details.statusCode !== 200 && details.statusCode !== 206) return null; // redirects report again when they land
    let url;
    try {
      url = new URL(details.url);
    } catch {
      return null;
    }
    if (!/^https?:$/.test(url.protocol) || EXCLUDED_HOSTS.test(url.hostname)) return null;

    const headers = details.responseHeaders;
    const mime = header(headers, "content-type").split(";")[0].trim().toLowerCase();
    const disposition = dispositionName(header(headers, "content-disposition"));
    const name = disposition || urlName(url);

    if (MANIFEST_MIME.test(mime) || (!NOT_MEDIA_MIME.test(mime) && MANIFEST_NAME.test(url.pathname))
        || (GENERIC_MIME.has(mime) && MANIFEST_NAME.test(name))) {
      return { stream: true, url: details.url, key: mediaKey(details.url), mime };
    }
    if (NOT_MEDIA_MIME.test(mime)) return null;
    if (PIECE_NAME.test(url.pathname)) return null;

    let format = MIME_FORMATS[mime] || "";
    let kind = format ? FORMATS[format] : "";
    if (!format && /^(video|audio)\//.test(mime)) {
      kind = mime.split("/")[0];
      const extension = extensionOf(name);
      format = FORMATS[extension] === kind ? extension : "";
    } else if (format && (format === "mp4" || format === "ogg")) {
      // A more exact extension for the same container: audio/mp4 named .m4a, video/mp4 named .m4v.
      const extension = extensionOf(name);
      if (FORMATS[extension] === kind && (extension === "m4v" || extension === "oga")) format = extension;
    }
    const isPlayer = details.type === "media";
    if (!format && !kind) {
      if (!GENERIC_MIME.has(mime)) return null; // some other specific type
      const extension = extensionOf(name);
      if (FORMATS[extension]) {
        format = extension;
        kind = FORMATS[extension];
      } else if (!isPlayer) {
        return null; // a generic type with nothing pointing to media
      } else {
        kind = "video"; // an element is playing it, so it is media of some kind
      }
    }

    const size = responseSize(headers);
    const isBackground = details.type === "xmlhttprequest";
    if (isBackground) {
      // Players fetching by script: only whole files, never the pieces of a stream.
      if (format === "ts" || NUMBERED_PIECE.test(url.pathname)) return null;
      if ([...url.searchParams.keys()].some((key) => RANGE_QUERY.test(key))) return null;
      if (size < MIN_BACKGROUND_SIZE) return null;
    } else if (size > 0 && size < MIN_PLAYER_SIZE) {
      return null;
    }

    return {
      url: details.url,
      key: mediaKey(details.url),
      kind,
      format,
      mime,
      name: fileNameFor(name, format, url.hostname),
      size,
      source: url.hostname,
      via: details.type,
    };
  }

  globalThis.HaulDetector = { classifyResponse, mediaKey, isMediaLink, fileNameFor, EXCLUDED_HOSTS };
})();
