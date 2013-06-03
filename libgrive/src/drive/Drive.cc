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

#include "Drive.hh"

#include "CommonUri.hh"
#include "Entry.hh"
#include "Feed.hh"

#include "http/Agent.hh"
#include "http/ResponseLog.hh"
#include "http/XmlResponse.hh"
#include "util/Destroy.hh"
#include "util/log/Log.hh"
#include "xml/Node.hh"
#include "xml/NodeSet.hh"

#include <boost/bind.hpp>

// standard C++ library
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

// for debugging only
#include <iostream>

namespace gr { namespace v1 {

namespace
{
	const std::string state_file = ".grive_state" ;
}

Drive::Drive( http::Agent *agent, const Json& options ) :
	m_http		( agent ),
	m_root		( options["path"].Str() ),
	m_state		( m_root / state_file, options ),
	m_options	( options )
{
	assert( m_http != 0 ) ;
}

void Drive::FromRemote( const Entry& entry )
{
	// entries from change feed does not have the parent HREF,
	// so these checkings are done in normal entries only
	Resource *parent = m_state.FindByHref( entry.ParentHref() ) ;
	
	if ( parent != 0 && !parent->IsFolder() )
		Log( "warning: entry %1% has parent %2% which is not a folder, ignored",
			entry.Title(), parent->Name(), log::verbose ) ;
	
	else if ( parent == 0 || !parent->IsInRootTree() )
		Log( "file \"%1%\" parent doesn't exist, ignored", entry.Title(), log::verbose ) ;
		
	else
		m_state.FromRemote( entry ) ;
}

void Drive::FromChange( const Entry& entry )
{
	if ( entry.IsRemoved() )
		Log( "file \"%1%\" represents a deletion, ignored", entry.Title(), log::verbose ) ;
	
	// folders go directly
	else
		m_state.FromRemote( entry ) ;
}

void Drive::SaveState()
{
	m_state.Write( state_file ) ;
}

void Drive::SyncFolders( )
{
	assert( m_http != 0 ) ;

	Log( "Synchronizing folders", log::info ) ;

	http::XmlResponse xml ;
	m_http->Get( feed_base + "/-/folder?max-results=50&showroot=true", &xml, http::Header() ) ;
	
	Feed feed( xml.Response() ) ;
	do
	{
		// first, get all collections from the query result
		for ( Feed::iterator i = feed.begin() ; i != feed.end() ; ++i )
		{
			Entry e( *i ) ;
			if ( e.Kind() == "folder" )
			{
				if ( e.ParentHrefs().size() != 1 )
					Log( "folder \"%1%\" has multiple parents, ignored", e.Title(), log::verbose ) ;
				
				else if ( e.Title().find('/') != std::string::npos )
					Log( "folder \"%1%\" contains a slash in its name, ignored", e.Title(), log::verbose ) ;
				
				else
					m_state.FromRemote( e ) ;
			}
		}
	} while ( feed.GetNext( m_http ) ) ;

	m_state.ResolveEntry() ;
}

void Drive::ImportEntryRemote( const Entry& a_entry )
{
	std::map<std::string, std::vector<Entry> >::iterator it_entry ;

	// Try to find the ParentHref in imported folders
	Resource *parent = m_state.FindByHref( a_entry.ParentHref() ) ;
	if (( parent == 0 ) && ( a_entry.ParentHref() != root_href ))
	{

		it_entry = a_map.find( a_entry.ParentHref() ) ;
		if ( it_entry != a_map.end() )
		{
            it_entry->second.push_back( a_entry ) ;
		}
		else
		{
			std::vector<Entry> new_vector ;
			new_vector.push_back( a_entry ) ;

			// if don't find it, put it on hold
			a_map[ a_entry.ParentHref() ] = new_vector ;
		}

	}
	else
	{
		m_state.Remote( a_entry, parent );

		// Try to find if the imported Entry is the parent of some folder on hold
        if ( a_entry.Kind() == "folder" )
        {
			it_entry = a_map.find( a_entry.SelfHref() );
			if ( it_entry != a_map.end() )
			{
				Entry m_entry;
				while ( !it_entry->second.empty() )
				{
					m_entry = it_entry->second.back();
					this->ImportEntryRemote( m_entry );
					it_entry->second.pop_back();
				}
				a_map.erase( it_entry );
			}
        }
	}
}

void Drive::BuildRemote( )
{
    Resource *parent ;
/*
    http::XmlResponse xml ;
	m_http->Get( feed_base + "/-/folder?max-results=50&showroot=true", &xml, http::Header() ) ;
	Feed feed( xml.Response() ) ;
*/

	Log( "Reading remote server file list", log::info ) ;
	Feed feed ;
	if ( m_options["log-xml"].Bool() )
		feed.EnableLog( "/tmp/file", ".xml" ) ;

	feed.Start( m_http, feed_base + "?showfolders=true&showroot=true" ) ;

	m_resume_link = feed.Root()["link"].
		Find( "@rel", "http://schemas.google.com/g/2005#resumable-create-media" )["@href"] ;

	do
	{
        // First, consider only the directories
    	for ( Feed::iterator i = feed.begin() ; i != feed.end() ; ++i )
		{
    		Entry r_entry( *i ) ;
    		if ( r_entry.ParentHrefs().size() != 1 )
				Log( "(folder) \"%1%\" has multiple parents, ignored", r_entry.Title(), log::verbose ) ;
			
			else if ( r_entry.Title().find('/') != std::string::npos )
				Log( "(folder) \"%1%\" contains a slash in its name, ignored", r_entry.Title(), log::verbose ) ;

            else
            {
            	this->ImportEntryRemote( r_entry );
            }
		}
	} while ( feed.GetNext( m_http ) ) ;
}

void Drive::DetectChanges()
{
	Log( "Reading local directories", log::info ) ;
	m_state.FromLocal( m_root ) ;
	
	long prev_stamp = m_state.ChangeStamp() ;
	Trace( "previous change stamp is %1%", prev_stamp ) ;
	
	SyncFolders( ) ;

	Log( "Reading remote server file list", log::info ) ;
	Feed feed ;
	if ( m_options["log-xml"].Bool() )
		feed.EnableLog( "/tmp/file", ".xml" ) ;
	
	feed.Start( m_http, feed_base + "?showfolders=true&showroot=true" ) ;
	
	m_resume_link = feed.Root()["link"].
		Find( "@rel", "http://schemas.google.com/g/2005#resumable-create-media" )["@href"] ;
		
	do
	{
		std::for_each(
			feed.begin(), feed.end(),
			boost::bind( &Drive::FromRemote, this, _1 ) ) ;
			
	} while ( feed.GetNext( m_http ) ) ;
	
	// pull the changes feed
	if ( prev_stamp != -1 )
	{
		Log( "Detecting changes from last sync", log::info ) ;
		Feed changes ;
		if ( m_options["log-xml"].Bool() )
			feed.EnableLog( "/tmp/changes", ".xml" ) ;
			
		feed.Start( m_http, ChangesFeed(prev_stamp+1) ) ;
		
		std::for_each(
			changes.begin(), changes.end(),
			boost::bind( &Drive::FromChange, this, _1 ) ) ;
	}
}

void Drive::Command_ls( const std::string& path, bool rec )
{
    Log( "Command ls: listing files (%1%)", path, log::verbose );
    m_state.ListingCmd( m_http, m_options, path, rec );
}

void Drive::Command_Download( const std::string& path,
                                 const std::string& format,
                                 const std::string& destination, bool rec )
{
    Log( "Command Download: Downloading %1% (%2%)", path, format, log::verbose );
    m_state.DownloadCmd( m_http, m_options, path, format, destination, rec );
}

void Drive::Command_Push( const std::string& path, const std::string& destination )
{
    Log( "Command Push: Pushing %1%", path, log::verbose );
    m_state.PushCmd( m_http, m_options, path, destination );
}

void Drive::Update()
{
	Log( "Synchronizing files", log::info ) ;
	m_state.Sync( m_http, m_options ) ;
	
	UpdateChangeStamp( ) ;
}

void Drive::DryRun()
{
	Log( "Synchronizing files (dry-run)", log::info ) ;
	m_state.Sync( 0, m_options ) ;
}

void Drive::UpdateChangeStamp( )
{
	assert( m_http != 0 ) ;

	// get changed feed
	http::XmlResponse xrsp ;
	m_http->Get( ChangesFeed(m_state.ChangeStamp()+1), &xrsp, http::Header() ) ;
	
	// we should go through the changes to see if it was really Grive to made that change
	// maybe by recording the updated timestamp and compare it?
	m_state.ChangeStamp( 
		std::atoi(xrsp.Response()["docs:largestChangestamp"]["@value"].front().Value().c_str()) ) ;
}

} } // end of namespace gr::v1
