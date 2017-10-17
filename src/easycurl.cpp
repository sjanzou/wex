/***********************************************************************************************************************
*  WEX, Copyright (c) 2008-2017, Alliance for Sustainable Energy, LLC. All rights reserved.
*
*  Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
*  following conditions are met:
*
*  (1) Redistributions of source code must retain the above copyright notice, this list of conditions and the following
*  disclaimer.
*
*  (2) Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the
*  following disclaimer in the documentation and/or other materials provided with the distribution.
*
*  (3) Neither the name of the copyright holder nor the names of any contributors may be used to endorse or promote
*  products derived from this software without specific prior written permission from the respective party.
*
*  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
*  INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
*  DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER, THE UNITED STATES GOVERNMENT, OR ANY CONTRIBUTORS BE LIABLE FOR
*  ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
*  PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
*  AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
*  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**********************************************************************************************************************/

#include <string>

#include <wx/wx.h>
#include <wx/ffile.h>
#include <wx/wfstream.h>
#include <wx/mstream.h>
#include <wx/buffer.h>
#include <wx/uri.h>
#include <wx/log.h>
#include <wx/progdlg.h>

#include <curl/curl.h>

#ifdef __WXMSW__
#include <winhttp.h>
#endif

#include <wex/label.h>
#include <wex/jsonreader.h>
#include <wex/utils.h>

#include <wex/easycurl.h>

#include <unordered_map>
using std::unordered_map;
#pragma warning(disable: 4290)  // ignore warning: 'C++ exception specification ignored except to indicate a function is not __declspec(nothrow)'

typedef unordered_map<wxString, wxString, wxStringHash, wxStringEqual> StringHash;

static wxString gs_curlProxyAddress;
static wxArrayString gs_curlProxyAutodetectMessages;
static StringHash gs_urlEscapes;

DEFINE_EVENT_TYPE(wxEASYCURL_EVENT);

extern "C" {
	int easycurl_progress_func(void* ptr, double rDlTotal, double rDlNow,
		double rUlTotal, double rUlNow);
	size_t easycurl_stream_write(void* ptr, size_t size, size_t nmemb, void* stream);
}; // extern "C"

class wxEasyCurl::DLThread : public wxThread
{
public:
	wxEasyCurl *m_sc;
	wxString m_url, m_proxy;
	CURLcode m_resultCode;

	wxMemoryBuffer m_data;
	wxMutex m_dataLock;

	bool m_threadDone;
	wxMutex m_threadDoneLock;

	bool m_canceled;
	wxMutex m_canceledLock;

	DLThread(wxEasyCurl *cobj, const wxString &url, const wxString &proxy)
		: wxThread(wxTHREAD_JOINABLE),
		m_sc(cobj),
		m_url(url),
		m_proxy(proxy),
		m_resultCode(CURLE_OK),
		m_threadDone(false),
		m_canceled(false)
	{
	}

	size_t Write(void *p, size_t len)
	{
		m_dataLock.Lock();

		// increase memory buffer size in 1 MB increments as needed
		if (m_data.GetDataLen() + len > m_data.GetBufSize())
			m_data.SetBufSize(m_data.GetBufSize() + 1048576);

		m_data.AppendData(p, len);

		m_dataLock.Unlock();
		return len;
	}

	wxString GetDataAsString()
	{
		wxString d;
		m_dataLock.Lock();

		wxStringOutputStream sstream(&d);
		sstream.Write(m_data.GetData(), m_data.GetDataLen());

		m_dataLock.Unlock();
		return d;
	}

	wxImage GetDataAsImage(wxBitmapType bittype)
	{
		m_dataLock.Lock();
		wxMemoryInputStream stream(m_data.GetData(), m_data.GetDataLen());
		wxImage img;
		img.LoadFile(stream, bittype);
		m_dataLock.Unlock();
		return img;
	}

	bool WriteDataToFile(const wxString &file)
	{
		m_dataLock.Lock();
		wxFFileOutputStream ff(file, "wb");
		if (ff.IsOk())
			ff.Write(m_data.GetData(), m_data.GetDataLen());
		m_dataLock.Unlock();
		return ff.IsOk();
	}

	virtual void *Entry()
	{
		if (CURL *curl = curl_easy_init())
		{
			struct curl_slist *chunk = NULL;

			wxArrayString headers = m_sc->m_httpHeaders;

			for (size_t i = 0; i < headers.size(); i++)
				chunk = curl_slist_append(chunk, (const char*)headers[i].c_str());

			curl_easy_setopt(curl, CURLOPT_HTTPHEADER, chunk);

			wxURI uri(m_url);
			wxString encoded = uri.BuildURI();
			curl_easy_setopt(curl, CURLOPT_URL, (const char*)encoded.c_str());

			if (!m_sc->m_postData.IsEmpty())
				curl_easy_setopt(curl, CURLOPT_POSTFIELDS, (const char*)m_sc->m_postData.c_str());

			curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
			curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

			curl_easy_setopt(curl, CURLOPT_WRITEDATA, this);
			curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, easycurl_stream_write);

			curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0);
			curl_easy_setopt(curl, CURLOPT_PROGRESSDATA, this);
			curl_easy_setopt(curl, CURLOPT_PROGRESSFUNCTION, easycurl_progress_func);

			curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1);

			if (!m_proxy.IsEmpty())
			{
				wxURI uri_proxy(m_proxy);
				wxString encoded_proxy = uri_proxy.BuildURI();
				curl_easy_setopt(curl, CURLOPT_PROXY, (const char*)encoded_proxy.ToAscii());
			}

			m_resultCode = curl_easy_perform(curl);
			curl_easy_cleanup(curl);

			/* free the custom headers */
			curl_slist_free_all(chunk);

			m_threadDoneLock.Lock();
			m_threadDone = true;
			m_threadDoneLock.Unlock();

			// issue finished event
			if (m_sc->m_handler != 0)
			{
				wxQueueEvent(m_sc->m_handler, new wxEasyCurlEvent(m_sc->m_id, wxEASYCURL_EVENT,
					wxEasyCurlEvent::FINISHED, "finished", m_url));
			}
		}

		return 0;
	}

	bool FinishedOk() {
		return m_resultCode == CURLE_OK;
	}

	wxString GetError() {
		return wxString(curl_easy_strerror(m_resultCode));
	}

	bool IsDone()
	{
		m_threadDoneLock.Lock();
		bool done = m_threadDone;
		m_threadDoneLock.Unlock();
		return done;
	}

	bool IsCanceled(){
		wxMutexLocker _ml(m_canceledLock);
		return m_canceled;
	}
	void Cancel()
	{
		wxMutexLocker _ml(m_canceledLock);
		m_canceled = true;
	}

	virtual bool TestDestroy() { return IsCanceled(); }

	void IssueProgressEvent(double rDlTotal, double rDlNow)
	{
		if (m_sc->m_handler != 0)
		{
			wxQueueEvent(m_sc->m_handler, new wxEasyCurlEvent(m_sc->m_id, wxEASYCURL_EVENT,
				wxEasyCurlEvent::PROGRESS, "progress", m_url, rDlNow, rDlTotal));
		}
	}
};

extern "C" {
	int easycurl_progress_func(void* userp, double rDlTotal, double rDlNow,
		double, double)
	{
		if (wxEasyCurl::DLThread *tt = static_cast<wxEasyCurl::DLThread*>(userp))
		{
			tt->IssueProgressEvent(rDlTotal, rDlNow);
			if (tt->IsCanceled())
				return -1; // return non zero should cancel
		}

		return 0;
	}

	size_t easycurl_stream_write(void* ptr, size_t size, size_t nmemb, void* userp)
	{
		wxEasyCurl::DLThread *tt = static_cast<wxEasyCurl::DLThread*>(userp);
		if (tt) return tt->Write(ptr, size*nmemb);
		else return 0;
	}
}; // extern "C"

wxEasyCurl::wxEasyCurl(wxEvtHandler *handler, int id)
	: m_thread(0), m_handler(handler),
	m_id(id)
{
}

wxEasyCurl::~wxEasyCurl()
{
	if (m_thread)
	{
		m_thread->Cancel();
		m_thread->Wait();
		delete m_thread;
	}
}

void wxEasyCurl::Start(const wxString &url)
{
	if (m_thread != 0)
	{
		m_thread->Cancel();
		m_thread->Wait();
		delete m_thread;
	}

	wxString escaped_url(url);

	for (StringHash::iterator it = gs_urlEscapes.begin();
		it != gs_urlEscapes.end();
		++it)
		escaped_url.Replace(it->first, it->second, true);

	m_thread = new DLThread(this, escaped_url, GetProxyForURL(escaped_url));
	m_thread->Create();
	m_thread->Run();
}

enum { ID_MY_SIMPLE_CURL = wxID_HIGHEST + 491 };

class SimpleCurlProgressDialog : public wxDialog
{
	wxLabel *m_label;
	wxGauge *m_gauge;
	wxString m_baseMsg;
	wxEasyCurl *m_simpleCurl;
	bool m_canceled;
public:
	SimpleCurlProgressDialog(wxEasyCurl *curl, wxWindow *parent, const wxString &msg)
		: wxDialog(parent, wxID_ANY, "Progress",
		wxDefaultPosition, wxScaleSize(500, 250), wxDEFAULT_DIALOG_STYLE | wxCLIP_CHILDREN)
	{
		SetBackgroundColour(*wxWHITE);

		m_canceled = false;
		m_simpleCurl = curl;
		m_baseMsg = msg;

		m_label = new wxLabel(this, wxID_ANY, msg + "                                                  ");
		m_label->SetBackgroundColour(GetBackgroundColour());
		m_gauge = new wxGauge(this, wxID_ANY, 100, wxDefaultPosition, wxDefaultSize, wxGA_SMOOTH);

		wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);
		sizer->Add(m_label, 0, wxALL | wxALIGN_LEFT, 10);
		sizer->Add(m_gauge, 0, wxALL | wxEXPAND | wxLEFT | wxRIGHT, 10);
		sizer->Add(CreateButtonSizer(wxCANCEL), 0, wxALL | wxEXPAND, 10);
		SetSizerAndFit(sizer);

		if (parent) CenterOnParent();
		else CenterOnScreen();
	}

	void OnSimpleCurlEvent(wxEasyCurlEvent &evt)
	{
		double bytes = evt.GetBytesTransferred();
		double total = evt.GetBytesTotal();
		double percent = total > 0 ? 100.0 * bytes / total : 0.0;
		if (percent > 100) percent = 100.0;

		m_label->SetText(m_baseMsg + "   " + wxString::Format("(%.2lf kB transferred)", bytes*0.001));
		m_gauge->SetValue((int)percent);
		Fit();

		if (m_canceled)
			m_simpleCurl->Cancel();
	}

	void OnButton(wxCommandEvent &evt)
	{
		m_canceled = true;
		if (wxWindow *win = dynamic_cast<wxWindow*>(evt.GetEventObject()))
			win->Enable(false);
	}

	void OnClose(wxCloseEvent &evt)
	{
		m_canceled = true;
		evt.Veto();
	}

	DECLARE_EVENT_TABLE()
};

BEGIN_EVENT_TABLE(SimpleCurlProgressDialog, wxDialog)
EVT_BUTTON(wxID_CANCEL, SimpleCurlProgressDialog::OnButton)
EVT_CLOSE(SimpleCurlProgressDialog::OnClose)
EVT_EASYCURL(ID_MY_SIMPLE_CURL, SimpleCurlProgressDialog::OnSimpleCurlEvent)
END_EVENT_TABLE()

bool wxEasyCurl::Get(const wxString &url, const wxString &msg, wxWindow *parent)
{
	bool show_progress = !msg.IsEmpty();

	SimpleCurlProgressDialog *pd = 0;
	if (show_progress)
	{
		if (!parent)
		{
			wxWindowList &wl = ::wxTopLevelWindows;
			for (wxWindowList::iterator it = wl.begin(); it != wl.end(); ++it)
				if (wxTopLevelWindow *tlw = dynamic_cast<wxTopLevelWindow*>(*it))
					if (tlw->IsActive())
						parent = tlw;
		}

		pd = new SimpleCurlProgressDialog(this, parent, msg);

#ifdef __WXMSW__
		pd->SetIcon(wxICON(appicon));
#endif
		pd->Show();
		wxYield();

		SetEventHandler(pd, ID_MY_SIMPLE_CURL);
	}

	Start(url);

	bool ok = Wait(show_progress);

	if (pd) delete pd;

	return ok;
}

bool wxEasyCurl::Wait(bool yield)
{
	while (1)
	{
		if (IsStarted() && !IsFinished())
		{
			if (yield) wxTheApp->Yield(true);
			wxMilliSleep(50);
		}
		else break;
	}

	return m_thread->FinishedOk();
}

void wxEasyCurl::SetEventHandler(wxEvtHandler *hh, int id)
{
	m_handler = hh;
	m_id = id;
}

bool wxEasyCurl::Ok()
{
	return (m_thread != 0 && m_thread->FinishedOk());
}

wxString wxEasyCurl::GetLastError()
{
	return m_thread != 0 ? m_thread->GetError() : (wxString)wxEmptyString;
}

bool wxEasyCurl::IsStarted()
{
	return (m_thread != 0 && !m_thread->IsDone());
}

wxString wxEasyCurl::GetDataAsString()
{
	return m_thread ? m_thread->GetDataAsString() : (wxString)wxEmptyString;
}

wxImage wxEasyCurl::GetDataAsImage(wxBitmapType bittype)
{
	return m_thread ? m_thread->GetDataAsImage(bittype) : wxNullImage;
}

bool wxEasyCurl::WriteDataToFile(const wxString &file)
{
	return m_thread ? m_thread->WriteDataToFile(file) : false;
}

bool wxEasyCurl::IsFinished()
{
	return (m_thread != 0 && m_thread->IsDone() && !m_thread->IsRunning());
}

void wxEasyCurl::Cancel()
{
	if (m_thread != 0)
		m_thread->Cancel();
}

void wxEasyCurl::SetProxyAddress(const wxString &proxy)
{
	gs_curlProxyAddress = proxy;
}

wxString wxEasyCurl::GetProxyForURL(const wxString &url)
{
	wxString proxy(gs_curlProxyAddress);

#ifdef __WXMSW__

	if (proxy.IsEmpty())
	{
		gs_curlProxyAutodetectMessages.Clear();

		// try to autodetect proxy address for url, at least on windows
		HINTERNET hInter = ::WinHttpOpen(L"", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);

		gs_curlProxyAutodetectMessages.Add("Querying IE proxy settings...");

		WINHTTP_CURRENT_USER_IE_PROXY_CONFIG cfg;
		memset(&cfg, 0, sizeof(WINHTTP_CURRENT_USER_IE_PROXY_CONFIG));
		if (::WinHttpGetIEProxyConfigForCurrentUser(&cfg))
		{
			printf("autoDetect? %s\n", cfg.fAutoDetect ? "yes" : "no");

			if (cfg.lpszAutoConfigUrl)
				gs_curlProxyAutodetectMessages.Add("lpszAutoConfigUrl: " + wxString(cfg.lpszAutoConfigUrl));

			if (cfg.lpszProxy)
				gs_curlProxyAutodetectMessages.Add("lpszProxy: " + wxString(cfg.lpszProxy));

			if (cfg.lpszProxyBypass)
				gs_curlProxyAutodetectMessages.Add("lpszProxyBypass: " + wxString(cfg.lpszProxyBypass));
		}
		else
		{
			gs_curlProxyAutodetectMessages.Add("Could not get IE proxy settings");
		}

		LPWSTR autoCfgUrl = cfg.lpszAutoConfigUrl;
		if (cfg.fAutoDetect || autoCfgUrl)
		{
			WINHTTP_AUTOPROXY_OPTIONS autoOpts;
			memset(&autoOpts, 0, sizeof(WINHTTP_AUTOPROXY_OPTIONS));
			autoOpts.fAutoLogonIfChallenged = TRUE;
			if (cfg.fAutoDetect)
			{
				autoOpts.dwAutoDetectFlags = WINHTTP_AUTO_DETECT_TYPE_DHCP | WINHTTP_AUTO_DETECT_TYPE_DNS_A;
				autoOpts.dwFlags = WINHTTP_AUTOPROXY_AUTO_DETECT;
			}
			if (autoCfgUrl)
			{
				autoOpts.lpszAutoConfigUrl = autoCfgUrl;
				autoOpts.dwFlags |= WINHTTP_AUTOPROXY_CONFIG_URL;
			}

			gs_curlProxyAutodetectMessages.Add("Querying auto configuration proxy for url: " + url);

			WINHTTP_PROXY_INFO autoCfg;
			memset(&autoCfg, 0, sizeof(WINHTTP_PROXY_INFO));
			if (::WinHttpGetProxyForUrl(hInter, url.ToStdWstring().c_str(),
				&autoOpts, &autoCfg)
				&& WINHTTP_ACCESS_TYPE_NO_PROXY != autoCfg.dwAccessType)
			{
				if (autoCfg.lpszProxy)
				{
					gs_curlProxyAutodetectMessages.Add("autoCfg.lpszProxy: " + wxString(autoCfg.lpszProxy));
					std::wstring wstr(autoCfg.lpszProxy);
					proxy = wxString(std::string(wstr.begin(), wstr.end()));
				}
				else
					gs_curlProxyAutodetectMessages.Add("No autodetected proxy determined");
			}
			else
			{
				gs_curlProxyAutodetectMessages.Add("Connection method does not use a proxy");
			}
		}
		else
		{
			gs_curlProxyAutodetectMessages.Add("Proxy autodetection disabled or no autoconfiguration url found");

			if (cfg.lpszProxy)
			{
				gs_curlProxyAutodetectMessages.Add("Using default: " + wxString(cfg.lpszProxy));
				std::wstring wstr(autoCfg.lpszProxy);
				proxy = wxString(std::string(wstr.begin(), wstr.end()));
			}
		}

		::WinHttpCloseHandle(hInter);
	}
#endif

	return proxy;
}

wxArrayString wxEasyCurl::GetProxyAutodetectMessages()
{
	return gs_curlProxyAutodetectMessages;
}

void wxEasyCurl::Initialize()
{
	::curl_global_init(CURL_GLOBAL_ALL);
}
void wxEasyCurl::Shutdown()
{
	::curl_global_cleanup();
}

void wxEasyCurl::SetUrlEscape(const wxString &key, const wxString &value)
{
	gs_urlEscapes[key] = value;
}

static wxString GOOGLE_API_KEY, BING_API_KEY;

void wxEasyCurl::SetApiKeys(const wxString &google, const wxString &bing)
{
	if (!google.IsEmpty()) GOOGLE_API_KEY = google;
	if (!bing.IsEmpty()) BING_API_KEY = bing;
}

// Google APIs:
// login to developer api console at: https://code.google.com/apis/console
// user name: aron.dobos@nrel.gov
// passwd: 1H*****....******r
//static wxString GOOGLE_API_KEY("AIzaSyCyH4nHkZ7FhBK5xYg4db3K7WN-vhpDxas");

// Bing Map APIs:
// login to account center at: https://www.bingmapsportal.com/
// user name: aron.dobos@nrel.gov
// passwd: 1H*****....******r
//static wxString BING_API_KEY("Av0Op8DvYGR2w07w_771JLum7-fdry0kBtu3ZA4uu_9jBJOUZgPY7mdbWhVjiORY");

bool wxEasyCurl::GeoCode(const wxString &address, double *lat, double *lon, double *tz)
{
	wxBusyCursor _curs;

	wxString plusaddr = address;
	plusaddr.Replace("   ", " ");
	plusaddr.Replace("  ", " ");
	plusaddr.Replace(" ", "+");

	wxString url("https://maps.googleapis.com/maps/api/geocode/json?address=" + plusaddr + "&sensor=false&key=" + GOOGLE_API_KEY);

	wxEasyCurl curl;
	wxBusyCursor curs;
	if (!curl.Get(url, "Geocoding address '" + address + "'..."))
		return false;

	wxJSONReader reader;
	wxJSONValue root;
	if (reader.Parse(curl.GetDataAsString(), &root) == 0)
	{
		wxJSONValue loc = root.Item("results").Item(0).Item("geometry").Item("location");
		if (!loc.IsValid()) return false;
		*lat = loc.Item("lat").AsDouble();
		*lon = loc.Item("lng").AsDouble();

		if (root.Item("status").AsString() != "OK")
			return false;
	}
	else
		return false;

	if (tz != 0)
	{
		// get timezone from another service
		url = wxString::Format("https://maps.googleapis.com/maps/api/timezone/json?location=%.14lf,%.14lf&timestamp=1&sensor=false&key=",
			*lat, *lon) + GOOGLE_API_KEY;
		bool ok = curl.Get(url, "Geocoding address...");
		if (ok && reader.Parse(curl.GetDataAsString(), &root) == 0)
		{
			wxJSONValue val = root.Item("rawOffset");
			if (val.IsDouble()) *tz = val.AsDouble() / 3600.0;
			else *tz = val.AsInt() / 3600.0;

			return root.Item("status").AsString() == "OK";
		}
		else
			return false;
	} // if no tz argument given then return true
	else return true;
}

wxBitmap wxEasyCurl::StaticMap(double lat, double lon, int zoom, MapProvider service)
{
	if (zoom > 21) zoom = 21;
	if (zoom < 1) zoom = 1;
	wxString zoomStr = wxString::Format("%d", zoom);

	wxString url;
	if (service == GOOGLE_MAPS)
	{
		url = "https://maps.googleapis.com/maps/api/staticmap?center="
			+ wxString::Format("%.9lf,%.9lf", lat, lon) + "&zoom=" + zoomStr
			+ "&size=800x800&maptype=hybrid&sensor=false&format=jpg-baseline&key=" + GOOGLE_API_KEY;
	}
	else
	{
		url = "http://dev.virtualearth.net/REST/v1/Imagery/Map/Aerial/"
			+ wxString::Format("%.15lf,%.15lf/%d", lat, lon, zoom)
			+ "?mapSize=800,800&format=jpeg&key=" + BING_API_KEY;
	}

	wxEasyCurl curl;
	bool ok = curl.Get(url, "Obtaining aerial imagery...");
	return ok ? wxBitmap(curl.GetDataAsImage(wxBITMAP_TYPE_JPEG)) : wxNullBitmap;
}
