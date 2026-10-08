#include "stdafx.h"
#include ".\dittowindow.h"
#include "CP_Main.h"
#include "Options.h"
#include <ShellScalingAPI.h>

/////////////////////////////////////////////////////////////////////////////
// Frosted "rice" support
//
// DWM treats pure black (0x00000000) pixels of an acrylic window as
// "unpainted", so while this mode is active all background fills are done
// with black and the current theme background color is used as the acrylic
// backdrop tint. The rounded corner preference is Windows 11 only; on older
// systems both effects are simply ignored.
/////////////////////////////////////////////////////////////////////////////

#define DITTO_RICE_ACCENT_ALPHA 0xD0

#define RICE_WCA_ACCENT_POLICY 19
#define RICE_ACCENT_STATE_DISABLED 0
#define RICE_ACCENT_STATE_ACRYLIC_BLURBEHIND 4
#define RICE_DWMWA_USE_IMMERSIVE_DARK_MODE 20
#define RICE_DWMWA_USE_IMMERSIVE_DARK_MODE_OLD 19
#define RICE_DWMWA_WINDOW_CORNER_PREFERENCE 33
#define RICE_DWMWA_SYSTEMBACKDROP_TYPE 38
#define RICE_DWMWCP_DEFAULT 0
#define RICE_DWMWCP_ROUND 2

// System backdrop types (DWMWA_SYSTEMBACKDROP_TYPE), Windows 11 22H2+.
#define RICE_DWMSBT_AUTO 1
#define RICE_DWMSBT_TRANSIENTWINDOW 4	// system acrylic, drawn inside the window's rounded corners

// Which compositing model a window's rice uses.
// ACCENT: undocumented accent acrylic, "GDI black" pixels show the frosted backdrop.
// BACKDROP: documented system backdrop; unpainted pixels show it, opaque paint covers it.
#define RICE_MODE_ACCENT 1
#define RICE_MODE_BACKDROP 2

// First Windows release with DWMWA_SYSTEMBACKDROP_TYPE (Windows 11 22H2).
#define RICE_SYSTEMBACKDROP_BUILD 22621

typedef struct _RICE_ACCENT_POLICY
{
	DWORD nAccentState;
	DWORD nFlags;
	DWORD nColor;
	DWORD nAnimationId;
} RICE_ACCENT_POLICY;

typedef struct _RICE_WINDOWCOMPOSITIONATTRIBDATA
{
	DWORD nAttribute;
	PVOID pData;
	SIZE_T ulDataSize;
} RICE_WINDOWCOMPOSITIONATTRIBDATA;

typedef BOOL(WINAPI *RICE_SETWINDOWCOMPOSITIONATTRIBUTE)(HWND, RICE_WINDOWCOMPOSITIONATTRIBDATA*);
typedef HRESULT(WINAPI *RICE_DWMSETWINDOWATTRIBUTE)(HWND, DWORD, LPCVOID, DWORD);
typedef HRESULT(WINAPI *RICE_DWMEXTENDFRAMEINTOCLIENTAREA)(HWND, LPCVOID);
typedef HRESULT(WINAPI *RICE_DWMENABLEBLURBEHINDWINDOW)(HWND, LPCVOID);

#define RICE_DWM_BB_ENABLE 0x00000001

typedef struct _RICE_DWM_BLURBEHIND
{
	DWORD dwFlags;
	BOOL fEnable;
	HRGN hRgnBlur;
	BOOL fTransitionOnMaximized;
} RICE_DWM_BLURBEHIND;

typedef struct _RICE_MARGINS
{
	int cxLeftWidth;
	int cxRightWidth;
	int cyTopHeight;
	int cyBottomHeight;
} RICE_MARGINS;

static RICE_DWMSETWINDOWATTRIBUTE RiceDwmSetWindowAttribute()
{
	static RICE_DWMSETWINDOWATTRIBUTE fn = NULL;
	static bool bLoaded = false;

	if (bLoaded == false)
	{
		bLoaded = true;
		HMODULE dwmapi = ::LoadLibrary(_T("dwmapi.dll"));
		if (dwmapi != NULL)
		{
			fn = (RICE_DWMSETWINDOWATTRIBUTE)::GetProcAddress(dwmapi, "DwmSetWindowAttribute");
		}
	}

	return fn;
}

static RICE_DWMEXTENDFRAMEINTOCLIENTAREA RiceDwmExtendFrameIntoClientArea()
{
	static RICE_DWMEXTENDFRAMEINTOCLIENTAREA fn = NULL;
	static bool bLoaded = false;

	if (bLoaded == false)
	{
		bLoaded = true;
		HMODULE dwmapi = ::LoadLibrary(_T("dwmapi.dll"));
		if (dwmapi != NULL)
		{
			fn = (RICE_DWMEXTENDFRAMEINTOCLIENTAREA)::GetProcAddress(dwmapi, "DwmExtendFrameIntoClientArea");
		}
	}

	return fn;
}

static RICE_DWMENABLEBLURBEHINDWINDOW RiceDwmEnableBlurBehindWindow()
{
	static RICE_DWMENABLEBLURBEHINDWINDOW fn = NULL;
	static bool bLoaded = false;

	if (bLoaded == false)
	{
		bLoaded = true;
		HMODULE dwmapi = ::LoadLibrary(_T("dwmapi.dll"));
		if (dwmapi != NULL)
		{
			fn = (RICE_DWMENABLEBLURBEHINDWINDOW)::GetProcAddress(dwmapi, "DwmEnableBlurBehindWindow");
		}
	}

	return fn;
}

static RICE_SETWINDOWCOMPOSITIONATTRIBUTE RiceSetWindowCompositionAttribute()
{
	static RICE_SETWINDOWCOMPOSITIONATTRIBUTE fn = NULL;
	static bool bLoaded = false;

	if (bLoaded == false)
	{
		bLoaded = true;
		HMODULE user32 = ::GetModuleHandle(_T("user32.dll"));
		if (user32 != NULL)
		{
			fn = (RICE_SETWINDOWCOMPOSITIONATTRIBUTE)::GetProcAddress(user32, "SetWindowCompositionAttribute");
		}
	}

	return fn;
}

static void RiceSetAccent(HWND hWnd, DWORD nAccentState, COLORREF color)
{
	RICE_SETWINDOWCOMPOSITIONATTRIBUTE setCompositionAttribute = RiceSetWindowCompositionAttribute();
	if (setCompositionAttribute == NULL)
	{
		return;
	}

	RICE_ACCENT_POLICY policy = { 0 };
	policy.nAccentState = nAccentState;
	policy.nFlags = 0;
	policy.nColor = ((DWORD)DITTO_RICE_ACCENT_ALPHA << 24) |
		((DWORD)GetBValue(color) << 16) |
		((DWORD)GetGValue(color) << 8) |
		(DWORD)GetRValue(color);

	RICE_WINDOWCOMPOSITIONATTRIBDATA data = { RICE_WCA_ACCENT_POLICY, &policy, sizeof(policy) };
	setCompositionAttribute(hWnd, &data);
}

bool DittoRiceEnabled()
{
	// Set DITTO_NO_RICE=1 in the environment to launch with the original solid look.
	static int s_nEnabled = -1;

	if (s_nEnabled < 0)
	{
		TCHAR value[8] = { 0 };
		DWORD nLength = ::GetEnvironmentVariable(_T("DITTO_NO_RICE"), value, 8);
		s_nEnabled = (nLength > 0 && value[0] == _T('1')) ? 0 : 1;
	}

	return s_nEnabled == 1;
}

bool DittoWindowIsRiced(HWND hWnd)
{
	if (hWnd == NULL)
	{
		return false;
	}

	HWND hRoot = ::GetAncestor(hWnd, GA_ROOT);
	if (hRoot == NULL)
	{
		hRoot = hWnd;
	}

	return ::GetProp(hRoot, _T("DittoRiceEnabled")) != NULL;
}

int DittoWindowRiceMode(HWND hWnd)
{
	if (hWnd == NULL)
	{
		return RICE_MODE_ACCENT;
	}

	HWND hRoot = ::GetAncestor(hWnd, GA_ROOT);
	if (hRoot == NULL)
	{
		hRoot = hWnd;
	}

	HANDLE hMode = ::GetProp(hRoot, _T("DittoRiceMode"));
	if (hMode == (HANDLE)(INT_PTR)RICE_MODE_BACKDROP)
	{
		return RICE_MODE_BACKDROP;
	}

	return RICE_MODE_ACCENT;
}

bool RiceOsSupportsSystemBackdrop()
{
	static bool s_bChecked = false;
	static bool s_bSupported = false;

	if (s_bChecked == false)
	{
		s_bChecked = true;

		typedef struct _RICE_OSVERSIONINFOEXW
		{
			ULONG dwOSVersionInfoSize;
			ULONG dwMajorVersion;
			ULONG dwMinorVersion;
			ULONG dwBuildNumber;
			ULONG dwPlatformId;
			WCHAR szCSDVersion[128];
			WORD wServicePackMajor;
			WORD wServicePackMinor;
			WORD wSuiteMask;
			WORD wProductType;
			WORD wReserved;
		} RICE_OSVERSIONINFOEXW;

		typedef LONG(WINAPI *RICE_RTLGETVERSION)(RICE_OSVERSIONINFOEXW*);

		HMODULE ntdll = ::GetModuleHandle(_T("ntdll.dll"));
		RICE_RTLGETVERSION rtlGetVersion = NULL;
		if (ntdll != NULL)
		{
			rtlGetVersion = (RICE_RTLGETVERSION)::GetProcAddress(ntdll, "RtlGetVersion");
		}

		if (rtlGetVersion != NULL)
		{
			RICE_OSVERSIONINFOEXW version = { 0 };
			version.dwOSVersionInfoSize = sizeof(version);
			if (rtlGetVersion(&version) == 0)
			{
				s_bSupported = (version.dwMajorVersion > 10) ||
					(version.dwMajorVersion == 10 && version.dwMinorVersion == 0 && version.dwBuildNumber >= RICE_SYSTEMBACKDROP_BUILD);
			}
		}
	}

	return s_bSupported;
}

void DittoRiceFillRect(CDC* pDC, const CRect& rect)
{
	if (pDC == NULL)
	{
		return;
	}

	// On the legacy accent acrylic black pixels show the frosted backdrop.
	// Fill them so stale pixels also clear. On the system backdrop the
	// unpainted pixels show the backdrop and any fill would cover it, so
	// painting the background is skipped entirely on that path.
	HWND hWnd = ::WindowFromDC(pDC->m_hDC);
	if (hWnd != NULL && DittoWindowRiceMode(hWnd) == RICE_MODE_BACKDROP)
	{
		return;
	}

	pDC->FillSolidRect(rect, RGB(0, 0, 0));
}

void ApplyRiceToWindow(HWND hWnd, COLORREF backgroundColor)
{
	if (hWnd == NULL || ::IsWindow(hWnd) == FALSE)
	{
		return;
	}

	if (DittoRiceEnabled() == false)
	{
		return;
	}

	// A layered window (the user's own transparency setting) cannot show the acrylic backdrop.
	if ((::GetWindowLong(hWnd, GWL_EXSTYLE) & WS_EX_LAYERED) != 0)
	{
		return;
	}

	RICE_DWMSETWINDOWATTRIBUTE dwmSetWindowAttribute = RiceDwmSetWindowAttribute();
	if (dwmSetWindowAttribute != NULL)
	{
		// The acrylic materials are built from the window's dark/light mode, so the
		// frosted look only reads correctly when the window is marked dark.
		BOOL bDarkMode = TRUE;
		dwmSetWindowAttribute(hWnd, RICE_DWMWA_USE_IMMERSIVE_DARK_MODE, &bDarkMode, sizeof(bDarkMode));
		dwmSetWindowAttribute(hWnd, RICE_DWMWA_USE_IMMERSIVE_DARK_MODE_OLD, &bDarkMode, sizeof(bDarkMode));

		DWORD nCornerPreference = RICE_DWMWCP_ROUND;
		dwmSetWindowAttribute(hWnd, RICE_DWMWA_WINDOW_CORNER_PREFERENCE, &nCornerPreference, sizeof(nCornerPreference));
	}

	// Window transparency helpers (Windhawk mods, older transparency paths) can
	// leave the legacy DWM blur-behind flag enabled, which switches the window to
	// per-pixel-alpha compositing and destroys the frosted backdrop. Clear it so
	// this re-application always starts from the plain acrylic state.
	RICE_DWMENABLEBLURBEHINDWINDOW dwmEnableBlurBehindWindow = RiceDwmEnableBlurBehindWindow();
	if (dwmEnableBlurBehindWindow != NULL)
	{
		RICE_DWM_BLURBEHIND blurBehind = { 0 };
		blurBehind.dwFlags = RICE_DWM_BB_ENABLE;
		blurBehind.fEnable = FALSE;
		dwmEnableBlurBehindWindow(hWnd, &blurBehind);
	}

	// Preferred path: the documented system acrylic backdrop (Windows 11 22H2+).
	// DWM renders the material inside the window's rounded corners itself, with no
	// edge or corner falloff, which is what the accent-acrylic corners show.
	// The backdrop state is zeroed by the same runtime events that reset the
	// accent state (fullscreen games, Windhawk mod unload/restore), so both the
	// frame extension and the backdrop type are set again on every re-apply.
	int nBackdropMode = RICE_MODE_ACCENT;

	RICE_DWMEXTENDFRAMEINTOCLIENTAREA dwmExtendFrame = RiceDwmExtendFrameIntoClientArea();
	if (dwmSetWindowAttribute != NULL && dwmExtendFrame != NULL && RiceOsSupportsSystemBackdrop())
	{
		RICE_MARGINS margins = { -1, -1, -1, -1 };
		if (dwmExtendFrame(hWnd, &margins) == 0)
		{
			int nBackdropType = RICE_DWMSBT_TRANSIENTWINDOW;
			if (dwmSetWindowAttribute(hWnd, RICE_DWMWA_SYSTEMBACKDROP_TYPE, &nBackdropType, sizeof(nBackdropType)) == 0)
			{
				nBackdropMode = RICE_MODE_BACKDROP;
			}
			else
			{
				// The attribute refused, so undo the extension to keep the
				// legacy fallback pixel-identical to its previous behaviour.
				RICE_MARGINS zero = { 0, 0, 0, 0 };
				dwmExtendFrame(hWnd, &zero);
			}
		}
	}

	::RemoveProp(hWnd, _T("DittoRiceMode"));
	::SetProp(hWnd, _T("DittoRiceEnabled"), (HANDLE)1);

	if (nBackdropMode == RICE_MODE_BACKDROP)
	{
		::SetProp(hWnd, _T("DittoRiceMode"), (HANDLE)(INT_PTR)RICE_MODE_BACKDROP);
	}
	else
	{
		// Legacy fallback (older systems): undocumented accent acrylic, where the
		// frame stays unextended and black client pixels show the backdrop.
		RiceSetAccent(hWnd, RICE_ACCENT_STATE_ACRYLIC_BLURBEHIND, backgroundColor);
	}
}

void RemoveRiceFromWindow(HWND hWnd)
{
	if (hWnd == NULL || ::IsWindow(hWnd) == FALSE)
	{
		return;
	}

	::RemoveProp(hWnd, _T("DittoRiceEnabled"));
	::RemoveProp(hWnd, _T("DittoRiceMode"));

	RICE_DWMSETWINDOWATTRIBUTE dwmSetWindowAttribute = RiceDwmSetWindowAttribute();
	RICE_DWMEXTENDFRAMEINTOCLIENTAREA dwmExtendFrame = RiceDwmExtendFrameIntoClientArea();

	if (dwmSetWindowAttribute != NULL)
	{
		DWORD nCornerPreference = RICE_DWMWCP_DEFAULT;
		dwmSetWindowAttribute(hWnd, RICE_DWMWA_WINDOW_CORNER_PREFERENCE, &nCornerPreference, sizeof(nCornerPreference));

		int nBackdropType = RICE_DWMSBT_AUTO;
		dwmSetWindowAttribute(hWnd, RICE_DWMWA_SYSTEMBACKDROP_TYPE, &nBackdropType, sizeof(nBackdropType));
	}

	if (dwmExtendFrame != NULL)
	{
		RICE_MARGINS zero = { 0, 0, 0, 0 };
		dwmExtendFrame(hWnd, &zero);
	}

	RiceSetAccent(hWnd, RICE_ACCENT_STATE_DISABLED, RGB(0, 0, 0));
}

CDittoWindow::CDittoWindow(void)
{
	m_captionBorderWidth = m_dpi.Scale(25);

	m_borderSize = 2;
	m_bMouseOverChevron = false;
	m_bMouseDownOnChevron = false;
	m_bMouseDownOnClose = false;
	m_bMouseOverClose = false;
	m_bMouseDownOnMinimize = false;
	m_bMouseOverMinimize = false;
	m_bMouseDownOnMaximize = false;
	m_bMouseOverMaximize = false;
	m_bDrawClose = true;
	m_bDrawChevron = true;
	m_bDrawMaximize = true;
	m_bDrawMinimize = true;
	m_bMinimized = false;
	m_crCloseBT.SetRectEmpty();
	m_crChevronBT.SetRectEmpty();
	m_crMaximizeBT.SetRectEmpty();
	m_crMinimizeBT.SetRectEmpty();
	m_CaptionColorLeft = RGB(255, 255, 255);
	m_CaptionColorRight = RGB(204, 204, 204);
	m_CaptionTextColor = RGB(191, 191, 191);
	m_border = RGB(204, 204, 204);
	m_sendWMClose = true;
	m_customWindowTitle = _T("");
	m_useCustomWindowTitle = false;
	m_buttonDownOnCaption = false;
	m_crFullSizeWindow.SetRectEmpty();	
	m_captionPosition = CAPTION_RIGHT;
	
}

CDittoWindow::~CDittoWindow(void)
{
}

void CDittoWindow::DoCreate(CWnd *pWnd)
{
	m_dpi.SetHwnd(pWnd->m_hWnd);
	

	m_VertFont.CreateFont(-m_dpi.Scale(19), 0, -900, 0, 400, FALSE, FALSE, 0, DEFAULT_CHARSET,
							OUT_DEFAULT_PRECIS,	CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, 
							DEFAULT_PITCH|FF_SWISS, _T("Segoe UI"));

	m_HorFont.CreateFont(-m_dpi.Scale(19), 0, 0, 0, 500, FALSE, FALSE, 0, DEFAULT_CHARSET,
						OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,
						DEFAULT_PITCH|FF_SWISS, _T("Segoe UI"));

	SetTitleTextHeight(pWnd);
	
	m_closeButton.LoadStdImageDPI(m_dpi.GetDPI(), Close_Black_16_16, Close_Black_20_20, Close_Black_24_24, Close_Black_28, Close_Black_32_32, _T("PNG"), close_36, close_40, close_44, close_48, close_52, close_56);
	m_chevronRightButton.LoadStdImageDPI(m_dpi.GetDPI(), ChevronRight_Black_16_16, ChevronRight_Black_20_20, ChevronRight_Black_24_24, ChevronRight_Black_28, ChevronRight_Black_32_32, _T("PNG"), ChevronRight_Black_36, ChevronRight_Black_40, ChevronRight_Black_44, ChevronRight_Black_48, ChevronRight_Black_52, ChevronRight_Black_56);
	m_chevronLeftButton.LoadStdImageDPI(m_dpi.GetDPI(), ChevronLeft_Black_16_16, ChevronLeft_Black_20_20, ChevronLeft_Black_24_24, ChevronLeft_Black_28, ChevronLeft_Black_32_32, _T("PNG"), ChevronLeft_Black_36, ChevronLeft_Black_40, ChevronLeft_Black_44, ChevronLeft_Black_48, ChevronLeft_Black_52, ChevronLeft_Black_56);
	m_maximizeButton.LoadStdImageDPI(m_dpi.GetDPI(), IDB_MAXIMIZE_16_16, maximize_20, maximize_24, maximize_28, maximize_32, _T("PNG"), maximize_36, maximize_40, maximize_44, maximize_48, maximize_52, maximize_56);
	m_minimizeButton.LoadStdImageDPI(m_dpi.GetDPI(), minimize_16, minimize_20, minimize_24, minimize_28, minimize_32, _T("PNG"), minimize_36, minimize_40, minimize_44, minimize_48, minimize_52, minimize_56);
	//m_windowIcon.LoadStdImageDPI(NewWindowIcon_24_14, NewWindowIcon_30, NewWindowIcon_36, NewWindowIcon_48, _T("PNG"));
}

void CDittoWindow::DoNcCalcSize(BOOL bCalcValidRects, NCCALCSIZE_PARAMS FAR* lpncsp)
{
	//Decrease the client area	
	if (m_captionPosition == CAPTION_LEFT)
		lpncsp->rgrc[0].left += m_captionBorderWidth;
	else
		lpncsp->rgrc[0].left += m_borderSize;

	if (m_captionPosition == CAPTION_TOP)
		lpncsp->rgrc[0].top += m_captionBorderWidth;
	else
		lpncsp->rgrc[0].top += m_borderSize;

	if (m_captionPosition == CAPTION_RIGHT)
		lpncsp->rgrc[0].right -= m_captionBorderWidth;
	else
		lpncsp->rgrc[0].right -= m_borderSize;

	if (m_captionPosition == CAPTION_BOTTOM)
		lpncsp->rgrc[0].bottom -= m_captionBorderWidth;
	else
		lpncsp->rgrc[0].bottom -= m_borderSize;
}

UINT CDittoWindow::DoNcHitTest(CWnd *pWnd, CPoint point) 
{
	CRect crWindow;
	pWnd->GetWindowRect(crWindow);

	if(crWindow.PtInRect(point) == false)
	{
		return -1;
	}
	
	int x = point.x - crWindow.left;
	int y = point.y - crWindow.top;

	CPoint myLocal(x, y);

	//http://stackoverflow.com/questions/521147/the-curious-problem-of-the-missing-wm-nclbuttonup-message-when-a-window-isnt-ma
	//workaround for l button up not coming after a lbutton down
	if (m_crCloseBT.PtInRect(myLocal) ||
		m_crChevronBT.PtInRect(myLocal) ||
		m_crMinimizeBT.PtInRect(myLocal) ||
		m_crMaximizeBT.PtInRect(myLocal))
	{
		return HTBORDER;;
	}

	if(m_bMinimized == false)
	{
		if ((point.y < crWindow.top + m_borderSize * 4) &&
			(point.x < crWindow.left + m_borderSize * 4))
			return HTTOPLEFT;
		else if ((point.y < crWindow.top + m_borderSize * 4) &&
			(point.x > crWindow.right - m_borderSize * 4))
			return HTTOPRIGHT;
		else if ((point.y > crWindow.bottom - m_borderSize * 4) &&
			(point.x > crWindow.right - m_borderSize * 4))
			return HTBOTTOMRIGHT;
		else if ((point.y > crWindow.bottom - m_borderSize * 4) &&
			(point.x < crWindow.left + m_borderSize * 4))
			return HTBOTTOMLEFT;
	}

	if((((m_captionPosition == CAPTION_TOP) || (m_captionPosition == CAPTION_BOTTOM)) &&
		(m_bMinimized)) == false)
	{
		if (point.y < crWindow.top + m_borderSize * 2)
			return HTTOP;
		if (point.y > crWindow.bottom - m_borderSize * 2)
			return HTBOTTOM;
	}

	if((((m_captionPosition == CAPTION_LEFT) || (m_captionPosition == CAPTION_RIGHT)) &&
		(m_bMinimized)) == false)
	{
		if (point.x > crWindow.right - m_borderSize * 2)
			return HTRIGHT;
		if (point.x < crWindow.left + m_borderSize * 2)
			return HTLEFT;
	}

	if (m_captionPosition == CAPTION_RIGHT)
	{
		if (point.x > crWindow.right - m_captionBorderWidth)
			return HTCAPTION;
	}
	if (m_captionPosition == CAPTION_BOTTOM)
	{
		if(point.y > crWindow.bottom - m_captionBorderWidth)
			return HTCAPTION;
	}
	if (m_captionPosition == CAPTION_LEFT)
	{
		if (point.x < crWindow.left + m_captionBorderWidth)
			return HTCAPTION;
	}
	if (m_captionPosition == CAPTION_TOP)
	{
		if (point.y < crWindow.top + m_captionBorderWidth)
			return HTCAPTION;
	}

	return -1;
}

int CDittoWindow::IndexToPos(int index, bool horizontal)
{
	switch (index)
	{
	case 0:
		if (horizontal)
		{
			return m_dpi.Scale(24);
		}
		else
		{
			return m_dpi.Scale(8);
		}
		break;
	case 1:
		if (horizontal)
		{
			return m_dpi.Scale(48);
		}
		else
		{
			return m_dpi.Scale(32);
		}
		break;
	case 2:

		if (horizontal)
		{
			return m_dpi.Scale(72);
		}
		else
		{
			return m_dpi.Scale(56);
		}
		break;
	case 3:
		if (horizontal)
		{
			return m_dpi.Scale(96);
		}
		else
		{
			return m_dpi.Scale(80);
		}
		break;
	case 4:
		if (horizontal)
		{
			return m_dpi.Scale(104);
		}
		else
		{
			return m_dpi.Scale(104);
		}
		break;
	}

	return 0;
}

void CDittoWindow::DoNcPaint(CWnd *pWnd)
{
	CWindowDC dc(pWnd);

	CRect rcFrame;
	pWnd->GetWindowRect(rcFrame);
	pWnd->ScreenToClient(rcFrame);

	CRect rc;
	pWnd->GetClientRect(rc);
	pWnd->ClientToScreen(rc);

	long lWidth = rcFrame.Width();

	// Draw the window border
	CRect rcBorder(0, 0, lWidth, rcFrame.Height());

	BOOL bRiceActive = DittoRiceEnabled() && DittoWindowIsRiced(pWnd->m_hWnd);

	int border = m_dpi.Scale(2);
	int widthHeight = m_dpi.Scale(16);

	if (bRiceActive == false)
	{
		for (int x = 0; x < border; x++)
		{
			dc.Draw3dRect(rcBorder, m_border, m_border);
			rcBorder.DeflateRect(1, 1, 1, 1);
		}
	}
	// In rice mode the frosted backdrop draws its own corner-fitting outline,
	// so the square GDI border is skipped: its square corners get clipped away
	// by the rounded window shape and leave broken, faded corner segments.
	rcBorder = CRect(0, 0, lWidth, rcFrame.Height());

	int iconArea = 0;
	int index = 0;
	int closeIndex = 0;
	int chevronIndex = 0;
	int minIndex = 0;
	int maxIndex = 0;

	if (m_bDrawClose)
	{
		iconArea += m_dpi.Scale(32);
		closeIndex = index++;
	}
	if (m_bDrawChevron)
	{
		iconArea += m_dpi.Scale(32);
		chevronIndex = index++;
	}
	if (m_bDrawMaximize)
	{
		iconArea += m_dpi.Scale(32);
		maxIndex = index++;
	}
	if (m_bDrawMinimize)
	{
		iconArea += m_dpi.Scale(32);
		minIndex = index++;
	}
	
	CRect leftRect;
	CRect rightRect;
	CRect textRect;	

	BOOL bVertical = FALSE;
	if(m_captionPosition == CAPTION_RIGHT)
	{
		rightRect.SetRect(rcBorder.right - (m_captionBorderWidth - border), rcBorder.top, rcBorder.right, rcBorder.top + IndexToPos(index, false));
		leftRect.SetRect(rcBorder.right - (m_captionBorderWidth - border), rcBorder.top + IndexToPos(index, false), rcBorder.right, rcBorder.bottom);
		
		textRect.SetRect(rcBorder.right, rightRect.bottom + m_dpi.Scale(10), rcBorder.right - m_captionBorderWidth, rcBorder.bottom - m_dpi.Scale(1));

		int left = rightRect.left;
		int right = rightRect.right;

		int top = IndexToPos(closeIndex, false);
		m_crCloseBT.SetRect(left, top, right, top+ widthHeight);

		top = IndexToPos(chevronIndex, false);
		m_crChevronBT.SetRect(left, top, right, top + widthHeight);

		top = IndexToPos(maxIndex, false);
		m_crMaximizeBT.SetRect(left, top, right, top + widthHeight);

		top = IndexToPos(minIndex, false);
		m_crMinimizeBT.SetRect(left, top, right, top + widthHeight);


		m_crWindowIconBT.SetRect(rcBorder.right - m_dpi.Scale(24), rcBorder.bottom - m_dpi.Scale(28), rcBorder.right - m_dpi.Scale(2), rcBorder.bottom);

		bVertical = TRUE;
	}
	if (m_captionPosition == CAPTION_LEFT)
	{
		rightRect.SetRect(rcBorder.left, rcBorder.top, rcBorder.left + m_captionBorderWidth - border, rcBorder.top + IndexToPos(index, false));
		leftRect.SetRect(rcBorder.left, rcBorder.top + IndexToPos(index, false), rcBorder.left + m_captionBorderWidth - border, rcBorder.bottom);

		textRect.SetRect(rcBorder.left + m_captionBorderWidth - m_dpi.Scale(0), rightRect.bottom + m_dpi.Scale(10), rcBorder.left - m_dpi.Scale(5), rcBorder.bottom - m_dpi.Scale(1));

		int left = rightRect.left;
		int right = rightRect.right;

		int top = IndexToPos(closeIndex, false);
		m_crCloseBT.SetRect(left, top, right, top + widthHeight);

		top = IndexToPos(chevronIndex, false);
		m_crChevronBT.SetRect(left, top, right, top + widthHeight);

		top = IndexToPos(maxIndex, false);
		m_crMaximizeBT.SetRect(left, top, right, top + widthHeight);

		top = IndexToPos(minIndex, false);
		m_crMinimizeBT.SetRect(left, top, right, top + widthHeight);

		m_crWindowIconBT.SetRect(rcBorder.left + m_dpi.Scale(0), rcBorder.bottom - m_dpi.Scale(28), rcBorder.left + m_dpi.Scale(25), rcBorder.bottom);

		bVertical = TRUE;
	}
	if (m_captionPosition == CAPTION_TOP)
	{
		leftRect.SetRect(rcBorder.left, rcBorder.top, rcBorder.right - IndexToPos(index-1, true)- m_dpi.Scale(8), m_captionBorderWidth);
		rightRect.SetRect(leftRect.right, rcBorder.top, rcBorder.right, m_captionBorderWidth);

		textRect.SetRect(leftRect.right, leftRect.top, leftRect.right, leftRect.bottom);

		int top = rightRect.top;
		int bottom = rightRect.bottom;

		int left = rcBorder.right - IndexToPos(closeIndex, true);
		m_crCloseBT.SetRect(left, top, left + widthHeight, bottom);

		left = rcBorder.right - IndexToPos(chevronIndex, true);
		m_crChevronBT.SetRect(left, top, left + widthHeight, bottom);
		
		left = rcBorder.right - IndexToPos(maxIndex, true);
		m_crMaximizeBT.SetRect(left, top, left + widthHeight, bottom);
		
		left = rcBorder.right - IndexToPos(minIndex, true);
		m_crMinimizeBT.SetRect(left, top, left + widthHeight, bottom);
				left = rcBorder.left + m_dpi.Scale(10);
		m_crWindowIconBT.SetRect(left, top, left + m_dpi.Scale(24), bottom);
		
		bVertical = FALSE;
	}
	if (m_captionPosition == CAPTION_BOTTOM)
	{
		leftRect.SetRect(rcBorder.left, rcBorder.bottom- m_captionBorderWidth - border, rcBorder.right - IndexToPos(index - 1, true) - m_dpi.Scale(8), rcBorder.bottom);
		rightRect.SetRect(leftRect.right, rcBorder.bottom - m_captionBorderWidth - border, rcBorder.right, rcBorder.bottom);

		textRect.SetRect(leftRect.right, leftRect.top, leftRect.right, leftRect.bottom);

		int top = rightRect.top;
		int bottom = rightRect.bottom;

		int left = rcBorder.right - IndexToPos(closeIndex, true);
		m_crCloseBT.SetRect(left, top, left + widthHeight, bottom);

		left = rcBorder.right - IndexToPos(chevronIndex, true);
		m_crChevronBT.SetRect(left, top, left+ widthHeight, bottom);

		left = rcBorder.right - IndexToPos(maxIndex, true);
		m_crMaximizeBT.SetRect(left, top, left + widthHeight, bottom);

		left = rcBorder.right - IndexToPos(minIndex, true);
		m_crMinimizeBT.SetRect(left, top, left + widthHeight, bottom);

		left = rcBorder.left + m_dpi.Scale(10);
		m_crWindowIconBT.SetRect(left, top, left + m_dpi.Scale(24), bottom);

		bVertical = FALSE;
	}


	if (bRiceActive)
	{
		// Leave the caption strip showing the frosted backdrop; the fill
		// helper paints blank-frost black only on the legacy accent path.
		DittoRiceFillRect(&dc, leftRect);
		DittoRiceFillRect(&dc, rightRect);
	}
	else
	{
		HBRUSH leftColor = CreateSolidBrush(m_CaptionColorLeft);
		HBRUSH rightColor = CreateSolidBrush(m_CaptionColorRight);

		::FillRect(dc, &leftRect, leftColor);
		::FillRect(dc, &rightRect, rightColor);

		DeleteObject(leftColor);
		DeleteObject(rightColor);
	}


	int nOldBKMode = dc.SetBkMode(TRANSPARENT);
	COLORREF oldColor = dc.SetTextColor(m_CaptionTextColor);

	CFont *pOldFont = NULL;
	if (bVertical)
		pOldFont = dc.SelectObject(&m_VertFont);
	else
		pOldFont = dc.SelectObject(&m_HorFont);

	CString csText = m_customWindowTitle;
	if (m_useCustomWindowTitle == false)
	{
		pWnd->GetWindowText(csText);
	}

	int flags = DT_SINGLELINE;
	if (bVertical == false)
	{
		CRect size(0, 0, 0, 0);
		dc.DrawText(csText, size, DT_CALCRECT);
		textRect.left = textRect.right - size.Width() - m_dpi.Scale(10);

		flags |= DT_VCENTER;
	}
	else
	{
		CRect size(0, 0, 0, 0);
		dc.DrawText(csText, size, DT_CALCRECT| DT_SINGLELINE);

		int rectWidth = textRect.left - textRect.right;
		int offset = rectWidth / 2 - m_titleTextHeight / 2;
		//textRect.right += 30;
		//I don't understand where the 4 is coming from but it's always 4 pixals from the right so adjust for this
		textRect.left -= (offset - m_dpi.Scale(4));		

		int k = 0;
	}

	dc.DrawText(csText, textRect, flags);

	dc.SelectObject(pOldFont);
	dc.SetBkMode(nOldBKMode);

	DrawWindowIcon(dc, pWnd);
	DrawChevronBtn(dc, pWnd);
	DrawCloseBtn(dc, pWnd);
	DrawMaximizeBtn(dc, pWnd);
	DrawMinimizeBtn(dc, pWnd);
}

void CDittoWindow::DrawChevronBtn(CWindowDC &dc, CWnd *pWnd)
{
	if(m_bDrawChevron == false)
	{
		return;
	}
		
	if(this->m_bMinimized)
	{
		m_chevronLeftButton.Draw(&dc, m_dpi, pWnd, m_crChevronBT, m_bMouseOverChevron, m_bMouseDownOnChevron);
	}
	else
	{
		m_chevronRightButton.Draw(&dc, m_dpi, pWnd, m_crChevronBT, m_bMouseOverChevron, m_bMouseDownOnChevron);
	}
}

void CDittoWindow::DrawWindowIcon(CWindowDC &dc, CWnd *pWnd)
{
	//m_windowIcon.Draw(&dc, pWnd, m_crWindowIconBT.left, m_crWindowIconBT.top, false, false);
}

void CDittoWindow::DrawCloseBtn(CWindowDC &dc, CWnd *pWnd)
{
	if(m_bDrawClose == false)
	{
		return;
	}
	
	m_closeButton.Draw(&dc, m_dpi, pWnd, m_crCloseBT, m_bMouseOverClose, m_bMouseDownOnClose);
}

void CDittoWindow::DrawMinimizeBtn(CWindowDC &dc, CWnd *pWnd)
{
	if(m_bDrawMinimize == false)
	{
		return;
	}

	m_minimizeButton.Draw(&dc, m_dpi, pWnd, m_crMinimizeBT, m_bMouseOverMinimize, m_bMouseDownOnMinimize);
}

void CDittoWindow::DrawMaximizeBtn(CWindowDC &dc, CWnd *pWnd)
{
	if(m_bDrawMaximize == false)
	{
		return;
	}

	m_maximizeButton.Draw(&dc, m_dpi, pWnd, m_crMaximizeBT, m_bMouseOverMaximize, m_bMouseDownOnMaximize);
}

int CDittoWindow::DoNcLButtonDown(CWnd *pWnd, UINT nHitTest, CPoint point) 
{
	switch (nHitTest)
	{
	case HTCAPTION:
		m_buttonDownOnCaption = true;
		break;
	default:
		m_buttonDownOnCaption = false;
	}

	int buttonPressed = 0;
	//ReleaseCapture();
	CPoint clPoint(point);
	pWnd->ScreenToClient(&clPoint);

	if (m_captionPosition == CAPTION_LEFT)
	{
		clPoint.x += m_captionBorderWidth;
	}
	else
	{
		clPoint.x += m_borderSize;
	}

	if (m_captionPosition == CAPTION_TOP)
	{
		clPoint.y += m_captionBorderWidth;
	}
	else
	{
		clPoint.y += m_borderSize;
	}	

	if(m_crCloseBT.PtInRect(clPoint))
	{
		m_bMouseDownOnClose = true;
		//InvalidateRect(pWnd->m_hWnd, m_crCloseBT, TRUE);
		//pWnd->InvalidateRect(m_crCloseBT);
		//pWnd->UpdateWindow();
		//DoNcPaint(pWnd);
		RedrawWindow(pWnd->m_hWnd, NULL, NULL, RDW_FRAME | RDW_INVALIDATE);
		buttonPressed = BUTTON_CLOSE;
	}
	else if(m_crChevronBT.PtInRect(clPoint))
	{
		m_bMouseDownOnChevron = true;
		RedrawWindow(pWnd->m_hWnd, NULL, NULL, RDW_FRAME | RDW_INVALIDATE);
		buttonPressed = BUTTON_CHEVRON;
	}
	else if(m_crMinimizeBT.PtInRect(clPoint))
	{
		m_bMouseDownOnMinimize = true;
		RedrawWindow(pWnd->m_hWnd, NULL, NULL, RDW_FRAME | RDW_INVALIDATE);
		buttonPressed = BUTTON_MINIMIZE;
	}
	else if(m_crMaximizeBT.PtInRect(clPoint))
	{
		m_bMouseDownOnMaximize = true;
		RedrawWindow(pWnd->m_hWnd, NULL, NULL, RDW_FRAME | RDW_INVALIDATE);
		buttonPressed = BUTTON_MAXIMIZE;
	}
	else if(m_bMinimized)
	{
		//MinMaxWindow(FORCE_MAX);
	}

	return buttonPressed;
}

long CDittoWindow::DoNcLButtonUp(CWnd *pWnd, UINT nHitTest, CPoint point) 
{
	m_buttonDownOnCaption = false;

	CRect crWindow;
	pWnd->GetWindowRect(crWindow);

	CPoint localPoint(point.x - crWindow.left, point.y - crWindow.top);

	long lRet = 0;
	if(m_bMouseDownOnClose)
	{
		m_bMouseDownOnClose = false;
		m_bMouseOverClose = false;

		RedrawWindow(pWnd->m_hWnd, NULL, NULL, RDW_FRAME | RDW_INVALIDATE);
		
		if(m_crCloseBT.PtInRect(localPoint))
		{
			if(m_sendWMClose)
			{
				pWnd->SendMessage(WM_CLOSE, 0, 0);
			}
			lRet = BUTTON_CLOSE;
		}
	}
	else if(m_bMouseDownOnChevron)
	{
		m_bMouseDownOnChevron = false;
		m_bMouseOverChevron = false;

		RedrawWindow(pWnd->m_hWnd, NULL, NULL, RDW_FRAME | RDW_INVALIDATE);

		if(m_crChevronBT.PtInRect(localPoint))
		{
			lRet = BUTTON_CHEVRON;
		}
	}
	else if(m_bMouseDownOnMinimize)
	{
		m_bMouseDownOnMinimize = false;
		m_bMouseOverMinimize = false;

		RedrawWindow(pWnd->m_hWnd, NULL, NULL, RDW_FRAME | RDW_INVALIDATE);

		if(m_crMinimizeBT.PtInRect(localPoint))
		{
			pWnd->ShowWindow(SW_MINIMIZE);
			lRet = BUTTON_MINIMIZE;
		}
	}
	else if(m_bMouseDownOnMaximize)
	{
		m_bMouseDownOnMaximize = false;
		m_bMouseOverMaximize = false;

		RedrawWindow(pWnd->m_hWnd, NULL, NULL, RDW_FRAME | RDW_INVALIDATE);

		if(m_crMaximizeBT.PtInRect(localPoint))
		{
			if(pWnd->GetStyle() & WS_MAXIMIZE)
				pWnd->ShowWindow(SW_RESTORE);
			else
				pWnd->ShowWindow(SW_SHOWMAXIMIZED);

			lRet = BUTTON_MAXIMIZE;
		}
	}

	return lRet;
}

void CDittoWindow::DoNcMouseMove(CWnd *pWnd, UINT nHitTest, CPoint point) 
{
	return;
	CRect crWindow;
	pWnd->GetWindowRect(crWindow);

	CPoint localPoint(point.x - crWindow.left, point.y - crWindow.top);

	if(m_crCloseBT.PtInRect(localPoint))
	{
		m_bMouseOverClose = true;
		RedrawWindow(pWnd->m_hWnd, NULL, NULL, RDW_FRAME | RDW_INVALIDATE);
	}
	else if(m_bMouseOverClose)
	{
		m_bMouseOverClose = false;
		RedrawWindow(pWnd->m_hWnd, NULL, NULL, RDW_FRAME | RDW_INVALIDATE);
	}

	if(m_crChevronBT.PtInRect(localPoint))
	{
		m_bMouseOverChevron = true;
		RedrawWindow(pWnd->m_hWnd, NULL, NULL, RDW_FRAME | RDW_INVALIDATE);
	}
	else if(m_bMouseOverChevron)
	{
		m_bMouseOverChevron = false;
		RedrawWindow(pWnd->m_hWnd, NULL, NULL, RDW_FRAME | RDW_INVALIDATE);
	}

	if(m_crMinimizeBT.PtInRect(localPoint))
	{
		m_bMouseOverMinimize = true;
		RedrawWindow(pWnd->m_hWnd, NULL, NULL, RDW_FRAME | RDW_INVALIDATE);
	}
	else if(m_bMouseOverMinimize)
	{
		m_bMouseOverMinimize = false;
		RedrawWindow(pWnd->m_hWnd, NULL, NULL, RDW_FRAME | RDW_INVALIDATE);
	}

	if(m_crMaximizeBT.PtInRect(localPoint))
	{
		m_bMouseOverMaximize = true;
		RedrawWindow(pWnd->m_hWnd, NULL, NULL, RDW_FRAME | RDW_INVALIDATE);
	}
	else if(m_bMouseOverMaximize)
	{
		m_bMouseOverMaximize = false;
		RedrawWindow(pWnd->m_hWnd, NULL, NULL, RDW_FRAME | RDW_INVALIDATE);
	}
}

bool CDittoWindow::DoPreTranslateMessage(MSG* pMsg) 
{
	return true;
}

void CDittoWindow::SetCaptionOn(CWnd *pWnd, int nPos, bool bOnstartup, int captionSize, int captionFontSize)
{
	m_captionFontSize = captionFontSize;

	m_VertFont.DeleteObject();
	m_VertFont.CreateFont(-m_dpi.Scale(captionFontSize), 0, -900, 0, 400, FALSE, FALSE, 0, DEFAULT_CHARSET,
		OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
		DEFAULT_PITCH | FF_SWISS, _T("Segoe UI"));

	m_HorFont.DeleteObject();
	m_HorFont.CreateFont(-m_dpi.Scale(captionFontSize), 0, 0, 0, 500, FALSE, FALSE, 0, DEFAULT_CHARSET,
		OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
		DEFAULT_PITCH | FF_SWISS, _T("Segoe UI"));

	SetTitleTextHeight(pWnd);

	m_captionPosition = nPos;

	int oldWidth = m_captionBorderWidth;
	m_captionBorderWidth = m_dpi.Scale(captionSize);	
		
	if(!bOnstartup)
	{
		pWnd->SetWindowPos(NULL, 0, 0, 0, 0, SWP_FRAMECHANGED|SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER);
	}

	pWnd->Invalidate();
	pWnd->RedrawWindow();

	if (oldWidth != m_captionBorderWidth)
	{
		::SetWindowPos(pWnd->m_hWnd, NULL, 0, 0, 0, 0, SWP_DRAWFRAME | SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);
	}
}

void CDittoWindow::SetTitleTextHeight(CWnd *pWnd)
{
	CWindowDC dc(pWnd);
	CFont *pOldFont = dc.SelectObject(&m_HorFont);
	CRect size(0, 0, 0, 0);
	dc.DrawText(_T("W"), size, DT_CALCRECT);
	m_titleTextHeight = size.Height();
	dc.SelectObject(pOldFont);
}

bool CDittoWindow::SetCaptionColors(COLORREF left, COLORREF right, COLORREF border)
{
	m_CaptionColorLeft = left;
	m_CaptionColorRight = right;
	m_border = border;

	return true;
}

void CDittoWindow::SetCaptionTextColor(COLORREF color)
{
	m_CaptionTextColor = color;
}

void CDittoWindow::MinMaxWindow(CWnd *pWnd, long lOption)
{
	if ((m_bMinimized) && (lOption == FORCE_MIN))
		return;

	if ((m_bMinimized == false) && (lOption == FORCE_MAX))
		return;

	if (m_captionPosition == CAPTION_RIGHT)
	{
		if (m_bMinimized == false)
		{
			pWnd->GetWindowRect(m_crFullSizeWindow);
			pWnd->MoveWindow(m_crFullSizeWindow.right - m_captionBorderWidth,
				m_crFullSizeWindow.top, m_captionBorderWidth,
				m_crFullSizeWindow.Height());
			m_bMinimized = true;
			m_TimeMinimized = COleDateTime::GetCurrentTime();
		}
		else
		{
			CRect cr;
			pWnd->GetWindowRect(cr);
			pWnd->MoveWindow(cr.right - m_crFullSizeWindow.Width(),
				cr.top, m_crFullSizeWindow.Width(), cr.Height());

			m_crFullSizeWindow.SetRectEmpty();
			m_bMinimized = false;
			m_TimeMaximized = COleDateTime::GetCurrentTime();
			::SetForegroundWindow(pWnd->GetSafeHwnd());
		}
	}
	if (m_captionPosition == CAPTION_LEFT)
	{
		if (m_bMinimized == false)
		{
			pWnd->GetWindowRect(m_crFullSizeWindow);
			pWnd->MoveWindow(m_crFullSizeWindow.left,
				m_crFullSizeWindow.top, m_captionBorderWidth,
				m_crFullSizeWindow.Height());
			m_bMinimized = true;
			m_TimeMinimized = COleDateTime::GetCurrentTime();
		}
		else
		{
			CRect cr;
			pWnd->GetWindowRect(cr);
			pWnd->MoveWindow(cr.left, cr.top,
				m_crFullSizeWindow.Width(), cr.Height());

			m_crFullSizeWindow.SetRectEmpty();
			m_bMinimized = false;
			m_TimeMaximized = COleDateTime::GetCurrentTime();
			::SetForegroundWindow(pWnd->GetSafeHwnd());
		}
	}
	if (m_captionPosition == CAPTION_TOP)
	{
		if (m_bMinimized == false)
		{
			pWnd->GetWindowRect(m_crFullSizeWindow);
			pWnd->MoveWindow(m_crFullSizeWindow.left,
				m_crFullSizeWindow.top,
				m_crFullSizeWindow.Width(),
				m_captionBorderWidth);
			m_bMinimized = true;
			m_TimeMinimized = COleDateTime::GetCurrentTime();
		}
		else
		{
			CRect cr;
			pWnd->GetWindowRect(cr);
			pWnd->MoveWindow(cr.left, cr.top,
				cr.Width(), m_crFullSizeWindow.Height());

			m_crFullSizeWindow.SetRectEmpty();
			m_bMinimized = false;
			m_TimeMaximized = COleDateTime::GetCurrentTime();
			::SetForegroundWindow(pWnd->GetSafeHwnd());
		}
	}
	if (m_captionPosition == CAPTION_BOTTOM)
	{
		if (m_bMinimized == false)
		{
			pWnd->GetWindowRect(m_crFullSizeWindow);
			pWnd->MoveWindow(m_crFullSizeWindow.left,
				m_crFullSizeWindow.bottom - m_captionBorderWidth,
				m_crFullSizeWindow.Width(),
				m_captionBorderWidth);
			m_bMinimized = true;
			m_TimeMinimized = COleDateTime::GetCurrentTime();
		}
		else
		{
			CRect cr;
			pWnd->GetWindowRect(cr);
			pWnd->MoveWindow(cr.left,
				cr.bottom - m_crFullSizeWindow.Height(),
				cr.Width(), m_crFullSizeWindow.Height());

			m_crFullSizeWindow.SetRectEmpty();
			m_bMinimized = false;
			m_TimeMaximized = COleDateTime::GetCurrentTime();
			::SetForegroundWindow(pWnd->GetSafeHwnd());
		}
	}
}

void CDittoWindow::OnDpiChanged(CWnd *pParent, int dpi)
{
	m_dpi.Update(dpi);

	m_captionBorderWidth = m_dpi.Scale(25);
	m_borderSize = m_dpi.Scale(2);

	m_VertFont.DeleteObject();
	m_HorFont.DeleteObject();

	m_VertFont.CreateFont(-m_dpi.Scale(m_captionFontSize), 0, -900, 0, 400, FALSE, FALSE, 0, DEFAULT_CHARSET,
		OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
		DEFAULT_PITCH | FF_SWISS, _T("Segoe UI"));

	m_HorFont.CreateFont(-m_dpi.Scale(m_captionFontSize), 0, 0, 0, 500, FALSE, FALSE, 0, DEFAULT_CHARSET,
		OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
		DEFAULT_PITCH | FF_SWISS, _T("Segoe UI"));

	m_closeButton.Reset();
	m_closeButton.LoadStdImageDPI(m_dpi.GetDPI(), Close_Black_16_16, Close_Black_20_20, Close_Black_24_24, Close_Black_28, Close_Black_32_32, _T("PNG"), close_36, close_40, close_44, close_48, close_52, close_56);

	m_chevronRightButton.Reset();
	m_chevronRightButton.LoadStdImageDPI(m_dpi.GetDPI(), ChevronRight_Black_16_16, ChevronRight_Black_20_20, ChevronRight_Black_24_24, ChevronRight_Black_28, ChevronRight_Black_32_32, _T("PNG"), ChevronRight_Black_36, ChevronRight_Black_40, ChevronRight_Black_44, ChevronRight_Black_48, ChevronRight_Black_52, ChevronRight_Black_56);
	
	m_chevronLeftButton.Reset();
	m_chevronLeftButton.LoadStdImageDPI(m_dpi.GetDPI(), ChevronLeft_Black_16_16, ChevronLeft_Black_20_20, ChevronLeft_Black_24_24, ChevronLeft_Black_28, ChevronLeft_Black_32_32, _T("PNG"), ChevronLeft_Black_36, ChevronLeft_Black_40, ChevronLeft_Black_44, ChevronLeft_Black_48, ChevronLeft_Black_52, ChevronLeft_Black_56);

	m_maximizeButton.Reset();
	m_maximizeButton.LoadStdImageDPI(m_dpi.GetDPI(), IDB_MAXIMIZE_16_16, maximize_20, maximize_24, maximize_28, maximize_32, _T("PNG"), maximize_36, maximize_40, maximize_44, maximize_48, maximize_52, maximize_56);

	m_minimizeButton.Reset();
	m_minimizeButton.LoadStdImageDPI(m_dpi.GetDPI(), minimize_16, minimize_20, minimize_24, minimize_28, minimize_32, _T("PNG"), minimize_36, minimize_40, minimize_44, minimize_48, minimize_52, minimize_56);

	SetTitleTextHeight(pParent);

	/*pParent->SetWindowPos(NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);

	pParent->Invalidate();
	pParent->RedrawWindow();*/

	//::SetWindowPos(pParent->m_hWnd, NULL, 0, 0, 0, 0, SWP_DRAWFRAME | SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);
}