// Typed helpers for the "WebView" controller (boson::ipc::WebViewController): print, PDF, capture,
// find-in-page, cookies and profile data of the calling WebView.
import { boson, type Unsubscribe } from './boson';

export interface PrintOptions {
  orientation?: 'portrait' | 'landscape';
  scale_factor?: number;
  page_width?: number;
  page_height?: number;
  margin_top?: number;
  margin_bottom?: number;
  margin_left?: number;
  margin_right?: number;
  print_backgrounds?: boolean;
  selection_only?: boolean;
  header_and_footer?: boolean;
  header_title?: string;
  footer_uri?: string;
  page_ranges?: string;
  pages_per_side?: 1 | 2 | 4 | 6 | 9 | 16;
  copies?: number;
  collation?: 'default' | 'collated' | 'uncollated';
  color_mode?: 'default' | 'color' | 'grayscale';
  duplex?: 'default' | 'one_sided' | 'two_sided_long_edge' | 'two_sided_short_edge';
  printer_name?: string;
}

export interface PrintResult {
  ok: boolean;
  status: 'succeeded' | 'printer_unavailable' | 'other_error';
}

export interface PdfResult {
  ok: boolean;
  path: string;
  data: string;
  size: number;
}

export interface CaptureResult {
  mime_type: string;
  data: string;
  path: string;
  size: number;
}

export interface FindOptions {
  term: string;
  case_sensitive?: boolean;
  whole_word?: boolean;
  highlight_all?: boolean;
  suppress_default_dialog?: boolean;
}

export interface FindStatus {
  match_count: number;
  active_match_index: number;
  active: boolean;
}

export type SameSite = 'none' | 'lax' | 'strict';

export interface Cookie {
  name: string;
  value: string;
  domain: string;
  path?: string;
  expires?: number | null;
  http_only?: boolean;
  secure?: boolean;
  same_site?: SameSite;
  readonly is_session?: boolean;
}

export type BrowsingDataKind =
  | 'file_systems' | 'indexed_db' | 'local_storage' | 'web_sql' | 'cache_storage' | 'all_dom_storage'
  | 'cookies' | 'all_site' | 'disk_cache' | 'download_history' | 'general_autofill' | 'password_autosave'
  | 'browsing_history' | 'settings' | 'all_profile' | 'service_workers';

export interface ProfileInfo {
  name: string;
  in_private: boolean;
  path: string;
  default_download_folder: string;
  color_scheme: 'auto' | 'light' | 'dark';
  tracking_prevention?: 'none' | 'basic' | 'balanced' | 'strict' | null;
}

interface Ok {
  ok: boolean;
}

/** One navigator.requestMediaKeySystemAccess query (webview::DrmKeySystemQuery). */
export interface DrmKeySystemQuery {
  /** Stable name for the result, e.g. "playready_sl3000". */
  id: string;
  key_system: string;
  /** Video robustness ("3000", "2000", "HW_SECURE_ALL", "SW_SECURE_DECODE"); empty = any. */
  robustness?: string;
  /** Audio robustness ("SW_SECURE_CRYPTO" for Widevine); empty = any. */
  audio_robustness?: string;
  /** true when "supported" means a hardware (protected media path) pipeline. */
  hardware?: boolean;
}

export interface DrmCodec {
  name: string;
  /** e.g. 'video/mp4; codecs="avc1.640028"' */
  content_type: string;
}

/** Every field is optional; omitted fields keep the native defaults. */
export interface DrmProbeOptions {
  key_systems?: DrmKeySystemQuery[];
  video_codecs?: DrmCodec[];
  /** Probed with a separate request after a video match; [] skips audio. */
  audio_codecs?: DrmCodec[];
  /** EME encryptionScheme values; default ['cenc', 'cbcs']; [] = unset. */
  encryption_schemes?: string[];
  /** Also call createMediaKeys() (initializes the CDM). Default true. */
  create_media_keys?: boolean;
  /** getStatusForPolicy({minHdcpVersion: '2.2'}) where available. Default true. */
  check_hdcp?: boolean;
  /** Per EME call, 1..60000. Default 10000. */
  timeout_ms?: number;
}

export interface DrmCapability {
  codec: string;
  content_type: string;
  encryption_scheme: string;
  robustness: string;
}

export interface DrmKeySystemResult {
  id: string;
  key_system: string;
  robustness: string;
  hardware: boolean;
  supported: boolean;
  video: DrmCapability[];
  audio: DrmCapability[];
  init_data_types: string[];
  session_types: string[];
  persistent_state: string;
  distinctive_identifier: string;
  /** null when not attempted. */
  media_keys_created?: boolean | null;
  /** MediaKeyStatus for HDCP 2.2, 'unsupported', 'error: ...' or '' when not checked. */
  hdcp_2_2: string;
  error: string;
  elapsed_ms: number;
}

export type DrmProtection = 'hardware' | 'software' | 'clearkey_only' | 'none';

export interface DrmProbeResult {
  secure_context: boolean;
  eme_available: boolean;
  origin: string;
  user_agent: string;
  key_systems: DrmKeySystemResult[];
  protection: DrmProtection;
  /** id of the first usable hardware result, else the first usable one; '' when none. */
  best: string;
  error: string;
}

/** Decodes a base64 payload returned by capture() / printToPdfData() into a Blob. */
export function base64ToBlob(data: string, type: string): Blob {
  const binary = atob(data);
  const bytes = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i++) {
    bytes[i] = binary.charCodeAt(i);
  }
  return new Blob([bytes], { type });
}

function call<R>(method: string, params?: object): Promise<R> {
  return boson.call<R, object>(method, params);
}

export const webview = {
  showPrintUI: (kind: 'browser' | 'system' = 'browser') => call<Ok>('WebView.show_print_ui', { kind }),
  print: (options: PrintOptions = {}) => call<PrintResult>('WebView.print', options),
  printToPdf: (path: string, settings: PrintOptions = {}) => call<PdfResult>('WebView.print_to_pdf', { path, settings }),
  printToPdfData: (options: PrintOptions = {}) => call<PdfResult>('WebView.print_to_pdf_data', options),
  /** Viewport screenshot; returns base64 data, or writes it to path when given. */
  capture: (format: 'png' | 'jpeg' = 'png', path?: string) =>
    call<CaptureResult>('WebView.capture', path === undefined ? { format } : { format, path }),

  findStart: (options: FindOptions) => call<FindStatus>('WebView.find_start', options),
  findNext: () => call<FindStatus>('WebView.find_next'),
  findPrevious: () => call<FindStatus>('WebView.find_previous'),
  findStop: () => call<FindStatus>('WebView.find_stop'),
  findStatus: () => call<FindStatus>('WebView.find_status'),
  /** Match updates, when the host forwards WebViewController::on_find_state as "webview:find". */
  onFind: (listener: (status: FindStatus) => void): Unsubscribe => boson.on<FindStatus>('webview:find', listener),

  getCookies: (uri = '') => call<Cookie[]>('WebView.get_cookies', { uri }),
  setCookie: (cookie: Cookie) => call<Ok>('WebView.set_cookie', cookie),
  deleteCookies: (name: string, target: { uri: string } | { domain: string; path?: string }) =>
    call<Ok>('WebView.delete_cookies', { name, ...target }),
  deleteAllCookies: () => call<Ok>('WebView.delete_all_cookies'),

  /** Empty kinds clears everything; times are UNIX seconds. */
  clearData: (kinds: BrowsingDataKind[] = [], startTime?: number, endTime?: number) =>
    call<Ok>('WebView.clear_data', {
      kinds,
      ...(startTime === undefined ? {} : { start_time: startTime }),
      ...(endTime === undefined ? {} : { end_time: endTime }),
    }),
  getProfile: () => call<ProfileInfo>('WebView.get_profile'),
  setColorScheme: (scheme: 'auto' | 'light' | 'dark') => call<Ok>('WebView.set_color_scheme', { scheme }),

  /**
   * EME capability probe of this page: PlayReady SL3000 / SL2000, Widevine HW_SECURE_ALL / SW_SECURE_DECODE and
   * Clear Key with avc1 / hev1 / vp09 / av01. Resolves which key systems, codecs and encryption schemes the
   * runtime grants and whether the CDM initializes; protection summarizes it ('hardware' = e.g. SL3000 active).
   */
  probeDrm: (options: DrmProbeOptions = {}) => call<DrmProbeResult>('WebView.probe_drm', options),
};
