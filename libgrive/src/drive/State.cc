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

#include "State.hh"

#include "Entry.hh"
#include "Resource.hh"
#include "CommonUri.hh"

#include "http/Agent.hh"
#include "util/Crypt.hh"
#include "util/File.hh"
#include "util/log/Log.hh"
#include "protocol/Json.hh"

#include <fstream>

namespace gr { namespace v1 {

State::State( const fs::path& filename, const Json& options  ) :
    m_res		( options["path"].Str() ),
	m_cstamp	( -1 )
{
	Read( filename ) ;
	
	// the "-f" option will make grive always thinks remote is newer
	Json force ;
	if ( options.Get("force", force) && force.Bool() )
		m_last_sync = DateTime() ;
	
	Log( "last sync time: %1%", m_last_sync, log::verbose ) ;
}

State::~State()
{
}

/// Synchronize local directory. Build up the resource tree from files and folders
/// of local directory.
void State::FromLocal( const fs::path& p )
{
	FromLocal( p, m_res.Root() ) ;
}

bool State::IsIgnore( const std::string& filename )
{
	return filename[0] == '.' ;
}

void State::FromLocal( const fs::path& p, Resource* folder )
{
	assert( folder != 0 ) ;
	assert( folder->IsFolder() ) ;
	
	// sync the folder itself
	folder->FromLocal( m_last_sync ) ;

	for ( fs::directory_iterator i( p ) ; i != fs::directory_iterator() ; ++i )
	{
		std::string fname = i->path().filename().string() ;
	
		if ( IsIgnore(fname) )
			Log( "file %1% is ignored by grive", fname, log::verbose ) ;
		
		// check for broken symblic links
		else if ( !fs::exists( i->path() ) )
			Log( "file %1% doesn't exist (broken link?), ignored", i->path(), log::verbose ) ;
		
		else
		{
			// if the Resource object of the child already exists, it should
			// have been so no need to do anything here
			Resource *c = folder->FindChild( fname ) ;
			if ( c == 0 )
			{
				c = new Resource( fname, fs::is_directory(i->path()) ? "folder" : "file" ) ;
				folder->AddChild( c ) ;
				m_res.Insert( c ) ;
			}
			
			c->FromLocal( m_last_sync ) ;			
			
			if ( fs::is_directory( i->path() ) )
				FromLocal( *i, c ) ;
		}
	}
}

void State::Remote( const Entry& e, Resource* folder )
{
    std::string fn = e.Filename() ;
    
    if ( folder == 0 )
        folder = m_res.Root();

    if ( IsIgnore( e.Name() ) )
		Log( "%1% %2% is ignored by grive", e.Kind(), e.Name(), log::verbose ) ;

	// common checkings
/*
    else if ( e.Kind() != "folder" && (fn.empty() || e.ContentSrc().empty()) )
		Log( "%1% \"%2%\" is a google document, ignored", e.Kind(), e.Name(), log::verbose ) ;
*/	
	else if ( fn.find('/') != fn.npos )
		Log( "%1% \"%2%\" contains a slash in its name, ignored", e.Kind(), e.Name(), log::verbose ) ;
	
	else if ( !e.IsChange() && e.ParentHrefs().size() != 1 )
		Log( "[%1%] \"%2%\" has multiple parents, ignored", e.Kind(), e.Title(), log::verbose ) ;

	else
    {
        Resource *c = new Resource( e.Name(), e.Kind() ) ;
        c->Remote( e );
		folder->AddChild( c ) ;
        m_res.Insert( c ) ;
    }
}

void State::FromRemote( const Entry& e )
{
	std::string fn = e.Filename() ;				

	if ( IsIgnore( e.Name() ) )
		Log( "%1% %2% is ignored by grive", e.Kind(), e.Name(), log::verbose ) ;

	// common checkings
	else if ( e.Kind() != "folder" && (fn.empty() || e.ContentSrc().empty()) )
		Log( "%1% \"%2%\" is a google document, ignored", e.Kind(), e.Name(), log::verbose ) ;
	
	else if ( fn.find('/') != fn.npos )
		Log( "%1% \"%2%\" contains a slash in its name, ignored", e.Kind(), e.Name(), log::verbose ) ;
	
	else if ( !e.IsChange() && e.ParentHrefs().size() != 1 )
		Log( "%1% \"%2%\" has multiple parents, ignored", e.Kind(), e.Name(), log::verbose ) ;

	else if ( e.IsChange() )
		FromChange( e ) ;

	else if ( !Update( e ) )
	{
		m_unresolved.push_back( e ) ;
	}
}

void State::ResolveEntry()
{
	while ( !m_unresolved.empty() )
	{
		if ( TryResolveEntry() == 0 )
			break ;
	}
}

std::size_t State::TryResolveEntry()
{
	assert( !m_unresolved.empty() ) ;

	std::size_t count = 0 ;
	std::vector<Entry>& en = m_unresolved ;
	
	for ( std::vector<Entry>::iterator i = en.begin() ; i != en.end() ; )
	{
		if ( Update( *i ) )
		{
			i = en.erase( i ) ;
			count++ ;
		}
		else
			++i ;
	}
	return count ;
}

void State::FromChange( const Entry& e )
{
	assert( e.IsChange() ) ;
	assert( !IsIgnore( e.Name() ) ) ;
	
	// entries in the change feed is always treated as newer in remote,
	// so we override the last sync time to 0
	if ( Resource *res = m_res.FindByHref( e.AltSelf() ) )
		m_res.Update( res, e, DateTime() ) ;
}

bool State::Update( const Entry& e )
{
	assert( !e.IsChange() ) ;
	assert( !e.ParentHref().empty() ) ;

	// Try to find the Entry in the RessourceTree
	if ( Resource *res = m_res.FindByHref( e.SelfHref() ) )
	{
		m_res.Update( res, e, m_last_sync ) ;
		return true ;
	}
	// Try to find the Parent Entry in the Entire RessourceTree
	else if ( Resource *parent = m_res.FindByHref( e.ParentHref() ) )
	{
		assert( parent->IsFolder() ) ;

		// see if the entry already exist in local
		std::string name = e.Name() ;
		Resource *child = parent->FindChild( name ) ;
		if ( child != 0 )
		{
			// since we are updating the ID and Href, we need to remove it and re-add it.
			m_res.Update( child, e, m_last_sync ) ;
		}
		
		// folder entry exist in google drive, but not local. we should create
		// the directory
		else if ( e.Kind() == "folder" || !e.Filename().empty() )
		{
			// first create a dummy resource and update it later
			child = new Resource( name, e.Kind() ) ;
			parent->AddChild( child ) ;
			m_res.Insert( child ) ;
			
			// update the state of the resource
			m_res.Update( child, e, m_last_sync ) ;
		}
		
		return true ;
	}
	else
		return false ;
}

Resource* State::FindByHref( const std::string& href )
{
	return m_res.FindByHref( href ) ;
}

State::iterator State::begin()
{
	return m_res.begin() ;
}

State::iterator State::end()
{
	return m_res.end() ;
}

void State::Read( const fs::path& filename )
{
	try
	{
		File file( filename ) ;
		Json json = Json::Parse( &file ) ;
		
		Json last_sync = json["last_sync"] ;
		m_last_sync.Assign(
			last_sync["sec"].Int(),
			last_sync["nsec"].Int() ) ;
		
		m_cstamp = json["change_stamp"].Int() ;
	}
	catch ( Exception& )
	{
		m_last_sync.Assign(0) ;
	}
}

void State::Write( const fs::path& filename ) const
{
	Json last_sync ;
	last_sync.Add( "sec",	Json(m_last_sync.Sec() ) );
	last_sync.Add( "nsec",	Json(m_last_sync.NanoSec() ) );
	
	Json result ;
	result.Add( "last_sync", last_sync ) ;
	result.Add( "change_stamp", Json(m_cstamp) ) ;
	
	std::ofstream fs( filename.string().c_str() ) ;
	fs << result ;
}

void State::Sync( http::Agent *http, const Json& options )
{
	// set the last sync time from the time returned by the server for the last file synced
	// if the sync time hasn't changed (i.e. now files have been uploaded)
	// set the last sync time to the time on the client
	// ideally because we compare server file times to the last sync time
	// the last sync time would always be a server time rather than a client time
	// TODO - WARNING - do we use the last sync time to compare to client file times
	// need to check if this introduces a new problem
 	DateTime last_sync_time = m_last_sync;
	m_res.Root()->Sync( http, last_sync_time, options ) ;
	
  	if ( last_sync_time == m_last_sync )
  	{
		Trace( "nothing changed? %1%", m_last_sync ) ;
    	m_last_sync = DateTime::Now();
  	}
  	else
  	{
		Trace( "updating last sync? %1%", last_sync_time ) ;
    	m_last_sync = last_sync_time;
  	}
}

Resource* State::FindResource( const std::string& path )
{

    // Parse "path" to find '/' and go to the correct directory recursively
    boost::filesystem::path given_path( path );
    boost::filesystem::path d_path;

    std::string v_string;
    Resource *element = 0;

    boost::filesystem::path::iterator it(given_path.begin());
    if ( *it == "/" )
    {
        it++;
        element = m_res.Root();
        for ( 1; it != given_path.end(); ++it )
        {
            d_path = *it;
            v_string = d_path.string();
            
            element = element->FindChild( v_string ) ;
            if ( element == 0)
            {
                Log( "!! Error: the child element hasn't been found.",
                        v_string, log::verbose );
                break;
            }
        }
    }
    else
    {
        Log( "!! Error: The Path must be relative to the root (must start with '/').",
                path, log::verbose );
    }

    return element ;
}

void State::ListingCmd( http::Agent *http, const Json& options,
                           const std::string& path, bool rec )
{
    
    Resource *element = this->FindResource( path ) ;
    if ( element != 0 )
    {
        element->Listing( http, options, rec );
    }
    else
    {
        Log( "!! Error: Resource '%1%' not found.", path, log::info );
    }

}

void State::DownloadCmd( http::Agent *http, const Json& options,
    	                    const std::string& path, const std::string& format,
                            const std::string& destination, bool rec )
{

    Resource *element = this->FindResource( path ) ;
    if ( element != 0 )
    {
        element->DownloadCmd( http, options, format, destination, rec, 1 );
    }
    else
    {
        Log( "!! Error: Resource '%1%' not found.", path, log::info );
    }

}

void State::PushCmd( http::Agent *http, const Json& options,
    	               const std::string& path, const std::string& destination )
{
    assert( !fs::is_directory( path ) ) ;
    
    boost::filesystem::path given_path( path ) ;
    std::string             filename  = given_path.filename().string() ;

    if ( !fs::exists( path ) )
    {
        Log( "!! Error: The file '%1%' doesn't exist on the filesystem.",
                path, log::info );
    }
    else
    {
        Resource *element = this->FindResource( destination ) ;
        if ( element != 0 )
        {
    		Resource *c = new Resource( filename, fs::is_directory( path ) ? "folder" : "file" ) ;
        	element->AddChild( c ) ;
    		m_res.Insert( c ) ;
            if ( c->PushCmd( http, options, path ) )
            {
                Log( "Upload of '%1%' done.", filename, log::info ) ;
            }
            else
            {
                Log( "!! Error: Not able to upload '%1%' in '%2%'.",
                        filename, destination, log::info ) ;
            }
        }
        else
        {
            Log( "!! Error: Destination directory '%1%' doesn't exist in GDrive.",
                    destination, log::info );
        }
    }
}

long State::ChangeStamp() const
{
	return m_cstamp ;
}

void State::ChangeStamp( long cstamp )
{
	Log( "change stamp is set to %1%", cstamp, log::verbose ) ;
	m_cstamp = cstamp ;
}

} } // end of namespace gr::v1
