#ifndef UICONTEXT_CMDLINEUICONTEXT_HPP
#define UICONTEXT_CMDLINEUICONTEXT_HPP

#include <memory>
#include <string>
#include <vector>
#include "appcontext/Timer.hpp"
#include "appcontext/OstreamTee.hpp"
#include "appcontext/UIContext.hpp"

namespace appcontext {
	struct CmdLineUIContext: public UIContext
	{
	public:
		CmdLineUIContext() ;
		~CmdLineUIContext() ;
	
		OstreamTee& logger() const ;
		ProgressContext get_progress_context( std::string const& name = "", std::string const& type = "bar" ) ;
	private:
		void remove_progress_context_impl( std::string const& name ) ;
	private:
		mutable std::map< std::string, ProgressContextImpl* > m_progress_contexts ;
		std::auto_ptr< OstreamTee > m_logger ;
	} ;


	struct ProgressBarProgressContext: public ProgressContextImpl
	{
	public:
		ProgressBarProgressContext( CmdLineUIContext const& ui_context, std::string const& name ) ;

		void notify_progress(
			std::size_t const count,
			std::size_t const total_count
		) const ;

		void finish() const ;

		std::string name() const { return m_name ; }

	private:
		void print_progress(
			std::size_t const count,
			std::size_t const total_count,
			std::string const& msg,
			std::size_t max_msg_length
		) const ;

		CmdLineUIContext const& m_ui_context ;
		std::string const m_name ;
		Timer m_timer ;
		mutable double m_last_time ;
	} ;
}

#endif