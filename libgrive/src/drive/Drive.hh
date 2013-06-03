/*
	grive: an GPL program to sync a local directory with Google Drive
	Copyright (C) 2012  Wan Wai Ho

	This program is free software; you can redistribute it and/or
	modify it under the terms of the GNU General Public License
	as published by the Free Software Foundation version 2
	of the License.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program; if not, write to the Free Software
	Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#pragma once

#include "State.hh"
#include "Entry.hh"

#include "http/Header.hh"
#include "protocol/Json.hh"
#include "util/Exception.hh"

#include <string>
#include <vector>
#include <map>

namespace gr {

namespace http
{
	class Agent ;
}

namespace v1 {

class Entry ;

class Drive
{
public :
	Drive( http::Agent *agent, const Json& options ) ;

	void DetectChanges() ;
	void BuildRemote( ) ;
	void Update() ;
	void DryRun() ;
	void SaveState() ;
	void Command_ls( const std::string& path, bool rec ) ;
	void Command_Download( const std::string& path, const std::string& format,
				const std::string& destination, bool rec );
	void Command_Push( const std::string& path, const std::string& destination );
	struct Error : virtual Exception {} ;
	
private :
	void SyncFolders( ) ;
    void file();
	void FromRemote( const Entry& entry ) ;
	void FromChange( const Entry& entry ) ;
	void UpdateChangeStamp( ) ;
	void ImportEntryRemote( const Entry& entry );
	
private :
	http::Agent 	*m_http ;
	std::string		m_resume_link ;
	fs::path		m_root ;
	State			m_state ;
	Json			m_options ;
	std::map<std::string, std::vector<Entry> > a_map ;
} ;

} } // end of namespace
