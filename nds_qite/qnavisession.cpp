#include "qnavisession.h"
#include "navisess_thread.h"
#include "NaviApiDll.hpp"
#include <QDebug>
#include <QMessageBox>
#include <tuple>

#ifdef WIN32
BOOL IsModuleLoaded(PCWSTR pszModuleName)
{
	// Get the module in the process according to the module name. 
	HMODULE hMod = GetModuleHandle(pszModuleName);
	return (hMod != NULL);
}
#else
#error "the DLL require the WIN32 platform"
#endif

namespace nsNaviSess
{
	enum SessState_E{
		SS_NONE,
		SS_REQ_CALC_ROUTE,
		SS_ACQ_ROUTE_RESULT,
		SS_REQ_EXTRACT_ROUTE_RESULT,
		SS_ACQ_EXTRACT_ROUTE_RESULT,
		SS_DONE
	};

	class CContext : public QSharedData
	{
	public:
		CContext(size_t sid) : m_sid(sid) {}
		CNdsNaviSession m_sess;
		const size_t m_sid;
	};

	inline QExplicitlySharedDataPointer<CContext> MakeContext(size_t sid)
	{
		return QExplicitlySharedDataPointer<CContext>(new CContext(sid));
	}

	class CExtractedRouteResultAcq : public CNaviSessAcquireBase
	{
	public:
		virtual void notify() final
		{}
	};

	class CExtractRouteReq : public CNaviSessRequestBase
	{
	public:
		virtual QExplicitlySharedDataPointer<CNaviSessAcquireBase> getSharedAquirement() final
		{
			return extractRoute();
		}
	private:
		QExplicitlySharedDataPointer<CNaviSessAcquireBase> extractRoute()
		{
			return MakeQExplicitSharedAcq<CExtractedRouteResultAcq>();
		}
	};

}

class CNaviSessCalcResultAcq : public CNaviSessAcquireBase
{
public:
	CNaviSessCalcResultAcq(QExplicitlySharedDataPointer<nsNaviSess::CContext> spCtx)
		: m_spContext(spCtx) {}
	virtual void notify() final
	{
		auto ins = QNaviSession::instance();
		ins->onAquireRouteCalcResult(m_spContext->m_sid);
	}
	QExplicitlySharedDataPointer<nsNaviSess::CContext> m_spContext;
};

class CNaviSessCalcRouteReq : public CNaviSessRequestBase
{
public:
	CNaviSessCalcRouteReq(size_t sid) : CNaviSessRequestBase(sid) {}
	virtual QExplicitlySharedDataPointer<CNaviSessAcquireBase> getSharedAquirement() final
	{
		return calcRoute();
	}
private:
	QExplicitlySharedDataPointer<CNaviSessAcquireBase> calcRoute()
	{
		auto pCtx = nsNaviSess::MakeContext(sessid);
		CNdsNaviSession& sess = pCtx->m_sess;

		if (sess.Initialize())
		{
			if (sess.calcRoute(
				1389087203, 476456031, // start pos never change
				//1388266368, 475637856	// only connective level
				1388572928, 476910336	// up level
				)){
				qDebug() << "SID[" << sessid << "] route OK";
			}
			else{
				qDebug() << "SID[" << sessid << "route failure!";
			}
		}

		return MakeQExplicitSharedAcq<CNaviSessCalcResultAcq>(pCtx);
	}
};

struct QNaviSession::CPrivate
{
	static QMutex mtx;
	CPrivate(QObject* parent)
		: sessid(0) 
		, mSessState(nsNaviSess::SS_NONE)
	{
#ifdef WIN32
		BOOL fLoaded = FALSE;
		// The name of the module to be delay-loaded. 
		PCWSTR pszModuleName = L"NaviApi";

		// Check whether or not the module is loaded. 
		fLoaded = IsModuleLoaded(pszModuleName);
		Q_ASSERT(fLoaded);
#endif

		pThread = new QNaviSessThread(parent);
		pThread->start();
	}

	~CPrivate() {
		if (pThread) delete pThread;
	}

	std::tuple<bool, bool, bool> update(nsNaviSess::SessState_E s)
	{
		QMutexLocker locker(&CPrivate::mtx);
		bool isUpdated = false;
		bool isBusy = false;
		bool isError = false;
		auto state_old = mSessState;

		switch (s)
		{
		case nsNaviSess::SS_REQ_CALC_ROUTE:
			if (nsNaviSess::SS_DONE == mSessState || nsNaviSess::SS_NONE == mSessState)
			{
				mSessState = nsNaviSess::SS_REQ_CALC_ROUTE;
				isUpdated = true;
			}
			else if (nsNaviSess::SS_REQ_CALC_ROUTE == mSessState)
			{
				isBusy = true;
			}
			else
			{
				isError = true;
			}
			break;
		case nsNaviSess::SS_ACQ_ROUTE_RESULT:
			if (nsNaviSess::SS_REQ_CALC_ROUTE == mSessState)
			{
				mSessState = nsNaviSess::SS_ACQ_ROUTE_RESULT;
				isUpdated = true;
			}
			else
			{
				isError = true;
			}
			break;
		case nsNaviSess::SS_REQ_EXTRACT_ROUTE_RESULT:
			if (nsNaviSess::SS_ACQ_ROUTE_RESULT)
			{
				mSessState = nsNaviSess::SS_REQ_EXTRACT_ROUTE_RESULT;
				isUpdated = true;
			}
			else if (nsNaviSess::SS_REQ_EXTRACT_ROUTE_RESULT == mSessState)
			{
				isBusy = true;
			}
			else
			{
				isError = true;
			}
			break;
		case nsNaviSess::SS_ACQ_EXTRACT_ROUTE_RESULT:
			if (nsNaviSess::SS_REQ_EXTRACT_ROUTE_RESULT == mSessState)
			{
				isUpdated = true;
			}
			else
			{
				isError = true;
			}
			break;
		case nsNaviSess::SS_DONE:
			switch (mSessState)
			{
			case nsNaviSess::SS_REQ_CALC_ROUTE:
			case nsNaviSess::SS_REQ_EXTRACT_ROUTE_RESULT:
				isBusy = true;
				break;
			case nsNaviSess::SS_ACQ_ROUTE_RESULT:
			case nsNaviSess::SS_ACQ_EXTRACT_ROUTE_RESULT:
				mSessState = nsNaviSess::SS_DONE;
				isUpdated = true;
				break;
			case nsNaviSess::SS_NONE:
			case nsNaviSess::SS_DONE:
			default:
				qDebug() << "do nothing";
				break;
			}
			break;
		default:
			break;
		}

		return std::make_tuple(isError, isBusy, isUpdated);
	}

	std::tuple<bool, bool, bool> stepOne()
	{
		QMutexLocker locker(&CPrivate::mtx);
		auto state_old = mSessState;
		locker.unlock();

		bool isErr, isBusy, isUpdated;
		isErr = isBusy = isUpdated = false;
		switch (state_old)
		{
		case nsNaviSess::SS_NONE:
		case nsNaviSess::SS_DONE:
			std::tie(isErr, isBusy, isUpdated) = update(nsNaviSess::SS_REQ_CALC_ROUTE);
			if (isUpdated)
			{
				pThread->sendReq(MakeQExplicitSharedReq<CNaviSessCalcRouteReq>(++sessid));
			}
			break;
		case nsNaviSess::SS_REQ_CALC_ROUTE:
			isBusy = true;
			break;
		case nsNaviSess::SS_ACQ_ROUTE_RESULT:
			std::tie(isErr, isBusy, isUpdated) = update(nsNaviSess::SS_REQ_EXTRACT_ROUTE_RESULT);
			if (isUpdated)
			{
				pThread->sendReq(MakeQExplicitSharedReq<nsNaviSess::CExtractRouteReq>());
			}
			break;
		default:
			qDebug() << __FUNCTIONW__ << "() unexpected session state";
			break;
		}

		return std::make_tuple(isErr, isBusy, isUpdated);
	}

	nsNaviSess::SessState_E mSessState;
	QNaviSessThread* pThread;
	size_t sessid;
};

QMutex QNaviSession::CPrivate::mtx;
QNaviSession::QNaviSession(void) : mp(new CPrivate(this))
{}

QNaviSession::~QNaviSession(void)
{
	delete mp;
}

QNaviSession* QNaviSession::instance()
{
	QMutexLocker locker(&CPrivate::mtx);
	static QNaviSession* pSess = new QNaviSession();
	return pSess;
}

void QNaviSession::onAquireRouteCalcResult(size_t sid)
{
	bool isErr, isUpdated, isBusy;
	std::tie(isErr, isBusy, isUpdated) = mp->update(nsNaviSess::SS_ACQ_ROUTE_RESULT);

	qDebug() << "SID[" << sid << "] : get the route result";
}

void QNaviSession::test()
{
	mp->stepOne();
}