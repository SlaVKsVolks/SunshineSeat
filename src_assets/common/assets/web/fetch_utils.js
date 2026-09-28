import { notifyKey } from './Notification.vue'

/**
 * The set of error messages that indicate a CSRF validation failure.
 */
const CSRF_ERRORS = new Set(['Missing CSRF token', 'Invalid CSRF token', 'CSRF token expired'])
let authRedirectInstalled = false

function isLocalApiRequest(input) {
  const requestUrl = typeof input === 'string' ? input : input?.url
  if (!requestUrl) return false

  if (requestUrl.startsWith('/api/')) return true
  if (requestUrl.startsWith('./api/')) return true

  try {
    const url = new URL(requestUrl, window.location.href)
    return url.origin === window.location.origin && url.pathname.startsWith('/api/')
  } catch {
    return false
  }
}

function isAuthRedirectExemptRequest(input) {
  try {
    const requestUrl = typeof input === 'string' ? input : input?.url
    if (!requestUrl) return false

    const url = new URL(requestUrl, window.location.href)
    return url.origin === window.location.origin && url.pathname === '/api/login'
  } catch {
    return false
  }
}

function redirectToLogin() {
  if (window.location.pathname !== '/login') {
    window.location.assign('/login')
  }
}

export function installApiAuthRedirect() {
  if (authRedirectInstalled) {
    return
  }

  const nativeFetch = window.fetch.bind(window)
  window.fetch = async (input, options) => {
    const response = await nativeFetch(input, options)
    if (response.status === 401 && isLocalApiRequest(input) && !isAuthRedirectExemptRequest(input)) {
      redirectToLogin()
    }
    return response
  }

  authRedirectInstalled = true
}

export async function getSessionState() {
  const response = await window.fetch('/api/session')
  if (!response.ok) {
    throw new Error(`Failed to fetch session state: ${response.status}`)
  }

  return response.json()
}

/**
 * Wrapper around the native fetch that automatically detects CSRF errors
 * (HTTP 400 with a known CSRF error message) and displays a notification.
 *
 * @param {string} url - The URL to fetch.
 * @param {RequestInit} [options] - Standard fetch options.
 * @returns {Promise<Response>} The fetch Response.
 */
export async function apiFetch(url, options) {
  const response = await fetch(url, options)

  if (response.status === 400) {
    let body = null
    try {
      body = await response.clone().json()
    } catch (e) {
      console.debug('apiFetch: response body is not JSON', e)
    }

    if (body && CSRF_ERRORS.has(body.error)) {
      notifyKey.error('_common.csrf_error_desc', '_common.csrf_error')
    }
  }

  return response
}
