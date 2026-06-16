"""Structured-log redaction helpers for the CorridorKey sidecar."""

import re

_MEDIA_EXTENSIONS = (
    "ari",
    "braw",
    "cin",
    "comp",
    "dng",
    "dpx",
    "drp",
    "exr",
    "flame",
    "json",
    "jpg",
    "jpeg",
    "log",
    "mov",
    "mp4",
    "mxf",
    "nk",
    "nuke",
    "npy",
    "png",
    "r3d",
    "txt",
    "tif",
    "tiff",
)

_MEDIA_EXT_RE = "|".join(_MEDIA_EXTENSIONS)
_FILE_URI_PATH_RE = re.compile(r"file://[^\s\"\n\r]+", re.IGNORECASE)
_UNC_PATH_RE = re.compile(r"\\\\[^\\\"\n\r]+\\[^\\\"\n\r]+(?:\\[^\\\"\n\r]+)*")
_WINDOWS_MEDIA_PATH_RE = re.compile(r"[A-Za-z]:\\[^\"\n\r]*\.(?:" + _MEDIA_EXT_RE + r")", re.IGNORECASE)
_UNIX_MEDIA_PATH_RE = re.compile(r"(?<![\w:/.])/[^\"\n\r]*\.(?:" + _MEDIA_EXT_RE + r")", re.IGNORECASE)
_RELATIVE_MEDIA_PATH_RE = re.compile(
    r"(?<![\w:/\\.-])(?:[^/\\\"\n\r]+[/\\])+[^/\\\"\n\r]+\.(?:" + _MEDIA_EXT_RE + r")(?![\w.-])",
    re.IGNORECASE,
)
_RELATIVE_PATH_RE = re.compile(r"(?<![\w:/\\.-])(?:[^/\\\"\n\r]+[/\\])+[^/\\\"\n\r]+")
_WINDOWS_PATH_RE = re.compile(r"(?<!\w)[A-Za-z]:\\(?:[^\\\"\n\r]+\\)+[^\\\"\n\r]+")
_UNIX_PATH_RE = re.compile(r"(?<![\w:/.])/(?:[^/\"\n\r]+/)+[^/\"\n\r]+")
_BARE_MEDIA_RE = re.compile(r"(?<![\w.-])[\w .-]+\.(?:" + _MEDIA_EXT_RE + r")(?![\w.-])", re.IGNORECASE)
_URL_RE = re.compile(r"[A-Za-z][A-Za-z0-9+.-]*://[^\s\"\n\r]+")
_LABELED_PROJECT_NAME_RE = re.compile(r"\b(project|show|shot)(\s*[:=]\s*|\s+)([^\n\r]*)", re.IGNORECASE)
_BARE_PROJECT_TOKEN_RE = re.compile(
    r"(?<![\w.-])(?:[A-Z][a-z0-9]+(?:[A-Z][A-Za-z0-9]+)+|[A-Za-z]+[._-]?[0-9]{2,}|[A-Z][A-Za-z0-9]*[._-][A-Za-z0-9._-]+|[A-Za-z0-9]+[._-][A-Z][A-Za-z0-9._-]*)(?![\w.-])"
)
_LABEL_VALUE_STOP_WORDS = {
    "available",
    "blocked",
    "cancelled",
    "completed",
    "failed",
    "failure",
    "missing",
    "ready",
    "retry",
    "running",
    "unavailable",
}
_BARE_PROJECT_TOKEN_ALLOWLIST = {
    "CorridorKey",
    "OpenFX",
    "DaVinci",
    "Resolve",
    "Flame",
    "Python",
    "PyTorch",
    "Torch",
    "OpenCV",
    "CMake",
    "CUDA",
    "ROCm",
    "Metal",
    "MLX",
    "MPS",
}

_SENSITIVE_NAME_KEYS = {
    "project",
    "project_name",
    "project_names",
    "show",
    "show_name",
    "show_names",
    "shot",
    "shot_name",
    "shot_names",
}

_SENSITIVE_PATH_KEYS = {
    "cache_path",
    "cache_dir",
    "clip_path",
    "destination_path",
    "file_path",
    "media_path",
    "output_path",
    "path",
    "project_path",
    "source_path",
}


def _normalize_key(key):
    if key is None:
        return None
    text = re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", str(key))
    return re.sub(r"[^a-z0-9]+", "_", text.lower()).strip("_")


def _is_sensitive_name_key(key_name):
    return key_name in _SENSITIVE_NAME_KEYS


def _is_sensitive_path_key(key_name):
    if key_name in _SENSITIVE_PATH_KEYS:
        return True
    return key_name.endswith("_path") or key_name.endswith("_paths") or key_name.endswith("_dir") or key_name.endswith("_dirs")


def _redact_bare_project_token(match):
    token = match.group(0)
    return token if token in _BARE_PROJECT_TOKEN_ALLOWLIST else "<redacted>"


def _redact_labeled_project_value(match):
    label, separator, value = match.groups()
    name_end = None
    for token_match in re.finditer(r"\S+", value):
        token = token_match.group(0).strip("\"'.,;:()[]{}").lower()
        if token == "<redacted>":
            return match.group(0)
        if token in _LABEL_VALUE_STOP_WORDS:
            break
        name_end = token_match.end()
    if name_end is None:
        return match.group(0)
    return f"{label}{separator}<redacted>{value[name_end:]}"


def redact_text(text):
    if not isinstance(text, str):
        return text

    redacted = _FILE_URI_PATH_RE.sub("<redacted-path>", text)
    redacted = _UNC_PATH_RE.sub("<redacted-path>", redacted)
    redacted = _WINDOWS_MEDIA_PATH_RE.sub("<redacted-path>", redacted)
    redacted = _RELATIVE_MEDIA_PATH_RE.sub("<redacted-path>", redacted)
    redacted = _UNIX_MEDIA_PATH_RE.sub("<redacted-path>", redacted)
    redacted = _WINDOWS_PATH_RE.sub("<redacted-path>", redacted)
    redacted = _UNIX_PATH_RE.sub("<redacted-path>", redacted)
    redacted = _RELATIVE_PATH_RE.sub("<redacted-path>", redacted)
    redacted = _LABELED_PROJECT_NAME_RE.sub(_redact_labeled_project_value, redacted)
    urls = []

    def protect_url(match):
        urls.append(match.group(0))
        return f"<redacted-url-{len(urls) - 1}>"

    redacted = _URL_RE.sub(protect_url, redacted)
    redacted = _BARE_MEDIA_RE.sub("<redacted-file>", redacted)
    redacted = _BARE_PROJECT_TOKEN_RE.sub(_redact_bare_project_token, redacted)
    for index, url in enumerate(urls):
        redacted = redacted.replace(f"<redacted-url-{index}>", url)
    return redacted


def redact_support_text(text):
    if not isinstance(text, str):
        return text
    return "\n".join(redact_text(line) for line in text.splitlines())


def redact_value(value, key=None):
    key_name = _normalize_key(key)

    if key_name is not None and _is_sensitive_name_key(key_name):
        if isinstance(value, list):
            return [redact_value(item, key) for item in value]
        if isinstance(value, tuple):
            return tuple(redact_value(item, key) for item in value)
        return "<redacted>"
    if key_name is not None and _is_sensitive_path_key(key_name):
        if isinstance(value, list):
            return [redact_value(item, key) for item in value]
        if isinstance(value, tuple):
            return tuple(redact_value(item, key) for item in value)
        return "<redacted-path>"
    if isinstance(value, dict):
        return {item_key: redact_value(item_value, item_key) for item_key, item_value in value.items()}
    if isinstance(value, list):
        return [redact_value(item, key) for item in value]
    if isinstance(value, tuple):
        return tuple(redact_value(item, key) for item in value)
    if isinstance(value, str):
        return redact_text(value)
    return value
