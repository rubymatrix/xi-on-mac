//! The PlayOnline session (../pol, C): sign in, get the FFXI session value V, keep the session up.

use std::ffi::{c_char, c_int, c_void, CStr, CString};

extern "C" {
    fn polffi_new() -> *mut c_void;
    fn polffi_signin(s: *mut c_void, host: *const c_char, pol_id: *const c_char, password: *const c_char) -> c_int;
    fn polffi_session_value(s: *mut c_void, v: *mut u8) -> c_int;
    fn polffi_pump(s: *mut c_void, timeout_ms: c_int) -> c_int;
    fn polffi_error(s: *const c_void) -> *const c_char;
    fn polffi_free(s: *mut c_void);
}

/// One sign-in. Dropping it signs out.
pub struct PolSession(*mut c_void);

// The handle is used by one thread at a time (it moves to the thread that runs the game).
unsafe impl Send for PolSession {}

impl PolSession {
    pub fn sign_in(host: &str, pol_id: &str, password: &str) -> Result<Self, String> {
        let s = unsafe { polffi_new() };
        if s.is_null() {
            return Err("out of memory".into());
        }
        let session = PolSession(s);
        let host = CString::new(host).map_err(|_| "bad server name")?;
        let id = CString::new(pol_id).map_err(|_| "bad PlayOnline ID")?;
        let pw = CString::new(password).map_err(|_| "bad password")?;
        if unsafe { polffi_signin(s, host.as_ptr(), id.as_ptr(), pw.as_ptr()) } == 0 {
            return Err(session.error());
        }
        Ok(session)
    }

    /// V, as the 32 hex digits host64 takes for --session.
    pub fn ffxi_session_value(&mut self) -> Result<String, String> {
        let mut v = [0u8; 16];
        if unsafe { polffi_session_value(self.0, v.as_mut_ptr()) } == 0 {
            return Err(self.error());
        }
        Ok(v.iter().map(|b| format!("{b:02x}")).collect())
    }

    /// Answers the server's keepalive; false once the connection has closed.
    pub fn pump(&mut self, timeout_ms: i32) -> bool {
        unsafe { polffi_pump(self.0, timeout_ms) != 0 }
    }

    fn error(&self) -> String {
        unsafe { CStr::from_ptr(polffi_error(self.0)) }.to_string_lossy().into_owned()
    }
}

impl Drop for PolSession {
    fn drop(&mut self) {
        unsafe { polffi_free(self.0) }
    }
}
