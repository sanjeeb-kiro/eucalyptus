/*******************************************************************************
 *Copyright (c) 2009  Eucalyptus Systems, Inc.
 * 
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, only version 3 of the License.
 * 
 * 
 *  This file is distributed in the hope that it will be useful, but WITHOUT
 *  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 *  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 *  for more details.
 * 
 *  You should have received a copy of the GNU General Public License along
 *  with this program.  If not, see <http://www.gnu.org/licenses/>.
 * 
 *  Please contact Eucalyptus Systems, Inc., 130 Castilian
 *  Dr., Goleta, CA 93101 USA or visit <http://www.eucalyptus.com/licenses/>
 *  if you need additional information or have any questions.
 * 
 *  This file may incorporate work covered under the following copyright and
 *  permission notice:
 * 
 *    Software License Agreement (BSD License)
 * 
 *    Copyright (c) 2008, Regents of the University of California
 *    All rights reserved.
 * 
 *    Redistribution and use of this software in source and binary forms, with
 *    or without modification, are permitted provided that the following
 *    conditions are met:
 * 
 *      Redistributions of source code must retain the above copyright notice,
 *      this list of conditions and the following disclaimer.
 * 
 *      Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 * 
 *    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 *    IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 *    TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 *    PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
 *    OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 *    EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 *    PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 *    PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 *    LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 *    NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 *    SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. USERS OF
 *    THIS SOFTWARE ACKNOWLEDGE THE POSSIBLE PRESENCE OF OTHER OPEN SOURCE
 *    LICENSED MATERIAL, COPYRIGHTED MATERIAL OR PATENTED MATERIAL IN THIS
 *    SOFTWARE, AND IF ANY SUCH MATERIAL IS DISCOVERED THE PARTY DISCOVERING
 *    IT MAY INFORM DR. RICH WOLSKI AT THE UNIVERSITY OF CALIFORNIA, SANTA
 *    BARBARA WHO WILL THEN ASCERTAIN THE MOST APPROPRIATE REMEDY, WHICH IN
 *    THE REGENTS’ DISCRETION MAY INCLUDE, WITHOUT LIMITATION, REPLACEMENT
 *    OF THE CODE SO IDENTIFIED, LICENSING OF THE CODE SO IDENTIFIED, OR
 *    WITHDRAWAL OF THE CODE CAPABILITY TO THE EXTENT NEEDED TO COMPLY WITH
 *    ANY SUCH LICENSES OR RIGHTS.
 *******************************************************************************
 * Author: chris grzegorczyk <grze@eucalyptus.com>
 */
package com.eucalyptus.auth;

import java.security.GeneralSecurityException;
import java.security.cert.X509Certificate;
import java.util.List;
import org.apache.log4j.Logger;
import org.hibernate.Session;
import org.hibernate.criterion.Example;
import org.hibernate.criterion.MatchMode;
import com.eucalyptus.auth.api.GroupProvider;
import com.eucalyptus.auth.api.NoSuchCertificateException;
import com.eucalyptus.auth.api.UserProvider;
import com.eucalyptus.auth.crypto.Crypto;
import com.eucalyptus.auth.crypto.Hmacs;
import com.eucalyptus.auth.principal.Group;
import com.eucalyptus.auth.principal.User;
import com.eucalyptus.auth.util.B64;
import com.eucalyptus.auth.util.PEMFiles;
import com.eucalyptus.entities.DatabaseUtil;
import com.eucalyptus.entities.EntityWrapper;
import com.eucalyptus.util.EucalyptusCloudException;
import com.google.common.collect.Lists;

public class DatabaseAuthProvider implements UserProvider, GroupProvider {
  private static Logger LOG = Logger.getLogger( DatabaseAuthProvider.class );
  
  DatabaseAuthProvider( ) {}
  
  @Override
  public User addUser( String userName, Boolean isAdmin, Boolean isEnabled ) throws UserExistsException {
    UserEntity newUser = new UserEntity( userName );
    newUser.setQueryId( Hmacs.generateQueryId( userName ) );
    newUser.setSecretKey( Hmacs.generateSecretKey( userName ) );
    newUser.setAdministrator( isAdmin );
    newUser.setEnabled( isEnabled );
    newUser.setPassword( Crypto.generateHashedPassword( userName ) );
    newUser.setToken( Crypto.generateSessionToken( userName ) );
    EntityWrapper<UserEntity> db = Authentication.getEntityWrapper( );
    try {
      db.add( newUser );
      db.commit( );
    } catch ( Throwable t ) {
      db.rollback( );
      throw new UserExistsException( t );
    }
    EntityWrapper<UserInfo> dbU = EntityWrapper.get( UserInfo.class );
    try {
      String confirmCode = Crypto.generateSessionToken( userName );
      UserInfo newUserInfo = new UserInfo( userName, confirmCode );
      dbU.add( newUserInfo );
      dbU.commit( );
    } catch ( Exception e ) {
      LOG.debug( e, e );
    }    
    return new UserProxy( newUser );
  }
  
  @Override
  public User deleteUser( String userName ) throws NoSuchUserException {
    UserEntity user = new UserEntity( userName );
    EntityWrapper<UserInfo> dbU = EntityWrapper.get( UserInfo.class );
    try {
      UserInfo newUserInfo = dbU.getUnique( new UserInfo( userName ) );
      dbU.delete( newUserInfo );
      dbU.commit( );
    } catch ( Exception e ) {
      LOG.debug( e, e );
    }    
    EntityWrapper<User> db = Authentication.getEntityWrapper( );
    try {
      User foundUser = db.getUnique( user );
      db.delete( foundUser );
      db.commit( );
      return foundUser;
    } catch ( Exception e ) {
      db.rollback( );
      throw new NoSuchUserException( e );
    }
  }
  
  @Override
  public List<Group> lookupUserGroups( User user ) {
    List<Group> userGroups = Lists.newArrayList( );
    EntityWrapper<GroupEntity> db = Groups.getEntityWrapper( );
    try {
      UserEntity userInfo = db.recast( UserEntity.class ).getUnique( new UserEntity( user.getName( ) ) );
      for ( GroupEntity g : db.query( new GroupEntity( ) ) ) {
        if ( g.belongs( userInfo ) ) {
          userGroups.add( new DatabaseWrappedGroup( g ) );
        }
      }
      db.commit( );
    } catch ( EucalyptusCloudException e ) {
      LOG.debug( e, e );
      db.rollback( );
    }
    return userGroups;
  }
  
  @Override
  public Group lookupGroup( String groupName ) throws NoSuchGroupException {
    EntityWrapper<GroupEntity> db = Groups.getEntityWrapper( );
    try {
      GroupEntity group = db.getUnique( new GroupEntity( groupName ) );
      db.commit( );
      return new DatabaseWrappedGroup( group );
    } catch ( EucalyptusCloudException e ) {
      db.rollback( );
      throw new NoSuchGroupException( e );
    }
  }
  
  @Override
  public List<User> listAllUsers( ) {
    List<User> users = Lists.newArrayList( );
    EntityWrapper<UserEntity> db = Authentication.getEntityWrapper( );
    UserEntity searchUser = new UserEntity( );
    searchUser.setEnabled( true );
    UserEntity user = null;
    try {
      users.addAll( db.query( searchUser ) );
      db.commit( );
    } catch ( Throwable e ) {
      db.rollback( );
    }
    return users;
  }
  
  @Override
  public List<User> listEnabledUsers( ) {
    List<User> users = Lists.newArrayList( );
    EntityWrapper<UserEntity> db = Authentication.getEntityWrapper( );
    UserEntity searchUser = new UserEntity( );
    searchUser.setEnabled( true );
    UserEntity user = null;
    try {
      users.addAll( db.query( searchUser ) );
      db.commit( );
    } catch ( Throwable e ) {
      db.rollback( );
    }
    return users;
  }
  
  @Override
  public User lookupCertificate( X509Certificate cert ) throws NoSuchUserException {
    String certPem = B64.url.encString( PEMFiles.getBytes( cert ) );
    UserEntity searchUser = new UserEntity( );
    searchUser.setCertificate( certPem );
    X509Cert searchCert = new X509Cert( );
    searchCert.setPemCertificate( certPem );
    searchUser.setEnabled( true );
    Session session = DatabaseUtil.getEntityManagerFactory( Authentication.DB_NAME ).getSessionFactory( ).openSession( );
    try {
      Example qbeUser = Example.create( searchUser ).enableLike( MatchMode.EXACT );
      Example qbeCert = Example.create( searchCert ).enableLike( MatchMode.EXACT );
      List<User> users = ( List<User> ) session.createCriteria( User.class ).setCacheable( true ).add( qbeUser ).createCriteria( "oldCertificates" )
                                               .setCacheable( true ).add( qbeCert ).list( );
      User ret = users.size( ) == 1 ? users.get( 0 ) : null;
      int size = users.size( );
      if ( ret != null ) {
        return ret;
      } else {
        throw new GeneralSecurityException( ( size == 0 ) ? "No user with the specified certificate." : "Multiple users with the same certificate." );
      }
    } catch ( Throwable t ) {
      throw new NoSuchUserException( t );
    } finally {
      try {
        session.close( );
      } catch ( Throwable t ) {}
    }
  }
  
  @Override
  public boolean checkRevokedCertificate( X509Certificate cert ) throws NoSuchCertificateException {
    String certPem = B64.url.encString( PEMFiles.getBytes( cert ) );
    UserEntity searchUser = new UserEntity( );
    X509Cert searchCert = new X509Cert( );
    searchCert.setPemCertificate( certPem );
    searchUser.setEnabled( true );
    Session session = DatabaseUtil.getEntityManagerFactory( Authentication.DB_NAME ).getSessionFactory( ).openSession( );
    try {
      Example qbeUser = Example.create( searchUser ).enableLike( MatchMode.EXACT );
      Example qbeCert = Example.create( searchCert ).enableLike( MatchMode.EXACT );
      List<User> users = ( List<User> ) session.createCriteria( User.class ).setCacheable( true ).add( qbeUser ).createCriteria( "oldCertificates" )
                                               .setCacheable( true ).add( qbeCert ).list( );
      if( users.isEmpty( ) || users.size( ) > 1 ) {
        session.close( );
        throw new NoSuchCertificateException( "Failed to identify user (found " + users.size() + ") from certificate information: " + cert.getSubjectX500Principal( ).toString( ) );
      } else {
        return true;
      }
    } finally {
      try {
        session.close( );
      } catch ( Throwable t ) {}
    }
  }
  
  
  @Override
  public User lookupQueryId( String queryId ) throws NoSuchUserException {
    String userName = null;
    EntityWrapper<UserEntity> db = Authentication.getEntityWrapper( );
    UserEntity searchUser = new UserEntity( );
    searchUser.setQueryId( queryId );
    UserEntity user = null;
    try {
      user = db.getUnique( searchUser );
      db.commit( );
    } catch ( Throwable e ) {
      db.rollback( );
      throw new NoSuchUserException( e );
    }
    return user;
  }
  
  @Override
  public User lookupUser( String userName ) throws NoSuchUserException {
    EntityWrapper<UserEntity> db = Authentication.getEntityWrapper( );
    UserEntity searchUser = new UserEntity( userName );
    UserEntity user = null;
    try {
      user = db.getUnique( searchUser );
      db.commit( );
    } catch ( Throwable e ) {
      db.rollback( );
      throw new NoSuchUserException( e );
    }
    return user;
  }
  
  @Override
  public Group addGroup( String groupName ) throws GroupExistsException {
    EntityWrapper<GroupEntity> db = Groups.getEntityWrapper( );
    GroupEntity newGroup = new GroupEntity( groupName );
    try {
      db.add( newGroup );
      db.commit( );
    } catch ( Throwable t ) {
      db.rollback( );
      throw new GroupExistsException( t );
    }
    return new DatabaseWrappedGroup( newGroup );
  }
  
  @Override
  public List<Group> listAllGroups( ) {
    List<Group> ret = Lists.newArrayList( );
    GroupEntity search = new GroupEntity( );
    EntityWrapper<GroupEntity> db = EntityWrapper.get( search );
    List<GroupEntity> groupList = db.query( search );
    for ( GroupEntity g : groupList ) {
      ret.add( new DatabaseWrappedGroup( g ) );
    }
    return ret;
  }

  @Override
  public void deleteGroup( String groupName ) throws NoSuchGroupException {
    EntityWrapper<GroupEntity> db = Groups.getEntityWrapper( );
    GroupEntity delGroup = new GroupEntity( groupName );
    try {
      GroupEntity g = db.getUnique( delGroup );
      db.delete( g );
      db.commit( );
    } catch ( Throwable t ) {
      db.rollback( );
      throw new NoSuchGroupException( t );
    }    
  }

  
  
}
