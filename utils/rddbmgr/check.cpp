// check.cpp
//
// Routines for --check for rddbmgr(8)
//
//   (C) Copyright 2018 Fred Gleason <fredg@paravelsystems.com>
//
//   This program is free software; you can redistribute it and/or modify
//   it under the terms of the GNU General Public License version 2 as
//   published by the Free Software Foundation.
//
//   This program is distributed in the hope that it will be useful,
//   but WITHOUT ANY WARRANTY; without even the implied warranty of
//   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//   GNU General Public License for more details.
//
//   You should have received a copy of the GNU General Public
//   License along with this program; if not, write to the Free Software
//   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
//

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <qdir.h>

#include <dbversion.h>
#include <rdcart.h>
#include <rdclock.h>
#include <rdcreate_log.h>
#include <rdcut.h>
#include <rddb.h>
#include <rdescape_string.h>
#include <rdhash.h>
#include <rdlog.h>
#include <rdwavefile.h>

#include "rddbmgr.h"

bool MainObject::Check(QString *err_msg) const
{
  if(GetCurrentSchema()!=RD_VERSION_DATABASE) {
    *err_msg="unsupported schema for checking";
    return false;
  }

  //
  // Recover Audio
  //
  if(!db_relink_audio.isEmpty()) {
    RelinkAudio(db_relink_audio);
    return true;
  }

  //
  // Check for Orphaned Voice Tracks
  //
  printf("Checking voice tracks...\n");
  CheckOrphanedTracks();
  printf("done.\n\n");

  //
  // Check RDLogManager Consistency
  //
  printf("Checking RDLogManager events...\n");
  CheckEvents();
  printf("done.\n\n");
  printf("Checking RDLogManager clocks...\n");
  CheckClocks();
  printf("done.\n\n");

  //
  // Check for orphaned tables
  //
  printf("Checking for orphaned tables...\n");
  CheckOrphanedTables();
  printf("done.\n\n");

  //
  // Check for stale reservations
  //
  printf("Checking for stale cart reservations...\n");
  CheckPendingCarts();
  printf("done.\n\n");

  //
  // Check for orphaned carts
  //
  printf("Checking for orphaned carts...\n");
  CheckOrphanedCarts();
  printf("done.\n\n");

  //
  // Check for orphaned cuts
  //
  printf("Checking for orphaned cuts...\n");
  CheckOrphanedCuts();
  printf("done.\n\n");

  //
  // Check Cart->Cut Counts
  //
  printf("Checking cart->cuts counters...\n");
  CheckCutCounts();
  printf("done.\n\n");

  //
  // Check Orphaned Audio
  //
  printf("Checking for orphaned audio...\n");
  CheckOrphanedAudio();
  printf("done.\n\n");

  //
  // Validating Audio Lengths
  //
  printf("Validating audio lengths (this may take some time)...\n");
  ValidateAudioLengths();
  printf("done.\n\n");

  //
  // Rehash
  //
  if(!db_rehash.isEmpty()) {
    printf("Checking hashes...\n");
    Rehash(db_rehash);
    printf("done.\n\n");
  }

  *err_msg="ok";
  return true;
}


void MainObject::RelinkAudio(const QString &srcdir) const
{
  QString sql;
  RDSqlQuery *q;

  QDir dir(srcdir);
  QStringList files=dir.entryList(QDir::Files|QDir::Readable|QDir::Hidden);
  for(unsigned i=0;i<files.size();i++) {
    QString filename=dir.path()+"/"+files[i];
    QString hash=RDSha1Hash(filename);
    QString firstdest;
    bool delete_source=true;
    sql=QString("select CUTS.CUT_NAME,CART.TITLE from ")+
      "CUTS left join CART "+
      "on CUTS.CART_NUMBER=CART.NUMBER where "+
      "CUTS.SHA1_HASH=\""+RDEscapeString(hash)+"\"";
    q=new RDSqlQuery(sql);
    while(q->next()) {
      printf("  Recovering %06u/%03d [%s]...",
	     RDCut::cartNumber(q->value(0).toString()),
	     RDCut::cutNumber(q->value(0).toString()),
	     (const char *)q->value(1).toString());
      fflush(stdout);
      if(db_relink_audio_move) {
	unlink(RDCut::pathName(q->value(0).toString()));
	if(link(filename,RDCut::pathName(q->value(0).toString()))<0) {
	  if(errno==EXDEV) {
	    if(firstdest.isEmpty()) {
	      if(CopyFile(RDCut::pathName(q->value(0).toString()),filename)) {
		firstdest=RDCut::pathName(q->value(0).toString());
	      }
	      else {
		fprintf(stderr,"unable to copy file \"%s\"\n",
			(const char *)filename);
		delete_source=false;
	      }
	    }
	    else {
	      unlink(RDCut::pathName(q->value(0).toString()));
	      link(firstdest,RDCut::pathName(q->value(0).toString()));
	    }
	  }
	  else {
	    fprintf(stderr,"unable to move file \"%s\" [%s]\n",
		    (const char *)filename,strerror(errno));
	    delete_source=false;
	  }
	}
      }
      else {
	if(firstdest.isEmpty()) {
	  if(CopyFile(RDCut::pathName(q->value(0).toString()),filename)) {
	    firstdest=RDCut::pathName(q->value(0).toString());
	  }
	  else {
	    fprintf(stderr,"unable to copy file \"%s\"\n",
		    (const char *)filename);
	  }
	}
	else {
	  unlink(RDCut::pathName(q->value(0).toString()));
	  link(firstdest,RDCut::pathName(q->value(0).toString()));
	}
      }
      printf("  done.\n");
    }
    if(db_relink_audio_move&&delete_source) {
      unlink(filename);
    }
  }
}


void MainObject::CheckOrphanedTracks() const
{
  QString logname;
  QString sql="select NUMBER,TITLE,OWNER from CART where OWNER!=\"\"";
  QSqlQuery *q=new QSqlQuery(sql);
  QSqlQuery *q1;

  while(q->next()) {
    logname=q->value(2).toString()+"_LOG";
    logname.replace(" ","_");
    sql=QString().sprintf("select ID from `%s` where CART_NUMBER=%u",
			  (const char *)logname,q->value(0).toUInt());
    q1=new QSqlQuery(sql);
    if(!q1->first()) {
      printf("  Found orphaned track %u - \"%s\".  Delete? (y/N) ",
	     q->value(0).toUInt(),(const char *)q->value(1).toString());
      fflush(NULL);
      if(UserResponse()) {
	RDCart *cart=new RDCart(q->value(0).toUInt());
	cart->remove(NULL,NULL,db_config);
	delete cart;
	RDLog *log=new RDLog(q->value(2).toString());
	if(log->exists()) {
	  log->updateTracks();
	}
	delete log;
      }
    }
    delete q1;
  }
  delete q;
}


void MainObject::CheckClocks() const
{
  QString sql;
  QSqlQuery *q;
  QSqlQuery *q1;
  QSqlQuery *q2;
  QSqlQuery *q3;
  QString eventname;

  sql="select NAME from CLOCKS";
  q=new QSqlQuery(sql);
  while(q->next()) {
    //
    // Check the CLK Table
    //
    sql=QString("select EVENT_NAME,ID from ")+
      RDClock::tableName(q->value(0).toString());
    q1=new QSqlQuery(sql);
    if(q1->isActive()) {
      //
      // Check the clock -> event linkage
      //
      while(q1->next()) {
	sql=QString().sprintf("select NAME from EVENTS where NAME=\"%s\"",
			      (const char *)q1->value(0).toString());
	q2=new QSqlQuery(sql);
	if(q2->first()) {
	  if(q1->value(0)!=q2->value(0)) {  // Make sure the cases match!
	    printf("  Clock %s's linkage to event %s is broken -- fix (y/N)? ",
		   (const char *)q->value(0).toString(),
		   (const char *)q2->value(0).toString());
	    fflush(NULL);
	    if(UserResponse()) {
	      sql=QString("update ")+
		RDClock::tableName(q->value(0).toString())+
		" set EVENT_NAME=\""+RDEscapeString(q2->value(0).toString())+
		"\""+QString().sprintf(" where ID=%d",q1->value(1).toInt());
	      q3=new QSqlQuery(sql);
	      delete q3;
	    }
	  }
	}
	delete q2;
      }
    }
    else {
      printf("  Clock %s is missing the CLK table -- fix (y/N)? ",
	     (const char *)q->value(0).toString());
      fflush(NULL);
      if(UserResponse()) {
	sql=RDCreateClockTableSql(RDClock::tableName(q->value(0).toString()),
				  db_config);
	q2=new QSqlQuery(sql);
	delete q2;
      }
    }
    delete q1;
  }
  delete q;
}


void MainObject::CheckEvents() const
{
  QString sql;
  QSqlQuery *q;
  QSqlQuery *q1;
  QSqlQuery *q2;
  QString eventname;

  sql="select NAME from EVENTS";
  q=new QSqlQuery(sql);
  while(q->next()) {
    eventname=q->value(0).toString();
    eventname.replace(" ","_");

    //
    // Check the PRE Table
    //
    sql=QString().sprintf ("select ID from %s_PRE",(const char *)eventname);
    q1=new QSqlQuery(sql);
    if(q1->size()<0) {
      printf("  Event %s is missing the PRE table -- fix (y/N)? ",
	     (const char *)q->value(0).toString());
      fflush(NULL);
      if(UserResponse()) {
	sql=RDCreateLogTableSql(eventname+"_PRE",db_config);
	q2=new QSqlQuery(sql);
	delete q2;
      }
    }
    delete q1;

    //
    // Check the POST Table
    //
    sql=QString().sprintf ("select ID from %s_POST",(const char *)eventname);
    q1=new QSqlQuery(sql);
    if(!q1->isActive()) {
      printf("  Event %s is missing the POST table -- fix (y/N)? ",
	     (const char *)q->value(0).toString());
      fflush(NULL);
      if(UserResponse()) {
	sql=RDCreateLogTableSql(eventname+"_POST",db_config);
	q2=new QSqlQuery(sql);
	delete q2;
      }
    }
    delete q1;
  }
  delete q;
}


void MainObject::CheckOrphanedTables() const
{
  QSqlQuery *table_q;
  QSqlQuery *q;
  QString sql;

  //
  // Generate Table Query
  //
  sql="show tables";
  table_q=new QSqlQuery(sql);

  //
  // Look for orphaned clocks
  //
  sql="select NAME from CLOCKS";
  q=new QSqlQuery(sql);
  CleanTables("CLK",table_q,q);
  CleanTables("RULES",table_q,q);
  delete q;

  //
  // Look for orphaned events
  //
  sql="select NAME from EVENTS";
  q=new QSqlQuery(sql);
  CleanTables("PRE",table_q,q);
  CleanTables("POST",table_q,q);
  delete q;

  //
  // Look for orphaned logs
  //
  sql="select NAME from LOGS";
  q=new QSqlQuery(sql);
  CleanTables("LOG",table_q,q);
  CleanTables("STACK",table_q,q);
  delete q;

  //
  // Look for orphaned services
  //
  sql="select NAME from SERVICES";
  q=new QSqlQuery(sql);
  CleanTables("SRT",table_q,q);
  delete q;

  //
  // Look for orphaned feeds
  //
  sql="select KEY_NAME from FEEDS";
  q=new QSqlQuery(sql);
  CleanTables("FIELDS",table_q,q);
  CleanTables("FLG",table_q,q);
  delete q;

  //
  // Look for stale and obsolete tables
  //
  CleanTables("IMP",table_q);
  CleanTables("REC",table_q);


  delete table_q;
}


void MainObject::CheckCutCounts() const
{
  QString sql;
  QSqlQuery *q;
  QSqlQuery *q1;

  sql="select NUMBER,CUT_QUANTITY,TITLE from CART";
  q=new QSqlQuery(sql);
  while(q->next()) {
    sql=QString().sprintf("select CUT_NAME from CUTS \
                           where (CART_NUMBER=%u)&&(LENGTH>0)",
			  q->value(0).toUInt());
    q1=new QSqlQuery(sql);
    if(q1->size()!=q->value(1).toInt()) {
      printf("  Cart %u [%s] has invalid cut count, fix (y/N)?",
	     q->value(0).toUInt(),(const char *)q->value(2).toString());
      if(UserResponse()) {
	RDCart *cart=new RDCart(q->value(0).toUInt());
	cart->updateLength();
	cart->resetRotation();
	delete cart;
      }
    }
    delete q1;
  }
  delete q;
}


void MainObject::CheckPendingCarts() const
{
  QString sql;
  QSqlQuery *q;
  QDateTime now(QDate::currentDate(),QTime::currentTime());

  sql=QString("select NUMBER from CART where ")+
    "(PENDING_STATION is not null)&&"+
    "(PENDING_DATETIME<\""+now.addDays(-1).
    toString("yyyy-MM-dd hh:mm:ss")+"\")";
  q=new QSqlQuery(sql);
  while(q->next()) {
    printf("  Cart %06u has stale reservation, delete cart(y/N)?",
	   q->value(0).toUInt());
    if(UserResponse()) {
      RDCart::removeCart(q->value(0).toUInt(),NULL,NULL,db_config);
    }
  }
  delete q;
}


void MainObject::CheckOrphanedCarts() const
{
  QString sql;
  QSqlQuery *q;
  QSqlQuery *q1;

  sql="select CART.NUMBER,CART.TITLE from CART left join GROUPS \
       on CART.GROUP_NAME=GROUPS.NAME where GROUPS.NAME is null";
  q=new QSqlQuery(sql);
  while(q->next()) {
    printf("  Cart %06u [%s] has missing/invalid group.\n",
	   q->value(0).toUInt(),(const char *)q->value(1).toString());
    if(db_orphan_group_name.isEmpty()) {
      printf("  Rerun rddbcheck with the --orphan-group= switch to fix.\n\n");
    }
    else {
      printf("  Assign to group \"%s\" (y/N)?",(const char *)db_orphan_group_name);
      if(UserResponse()) {
	sql=QString().
	  sprintf("update CART set GROUP_NAME=\"%s\" where NUMBER=%u",
		  (const char *)RDEscapeString(db_orphan_group_name),
		  q->value(0).toUInt());
	q1=new QSqlQuery(sql);
	delete q1;
      }
      printf("\n");
    }
  }
  delete q;
}


void MainObject::CheckOrphanedCuts() const
{
  QString sql;
  QSqlQuery *q;
  QSqlQuery *q1;
  QSqlQuery *q2;
  QFileInfo *file=NULL;

  sql="select CUTS.CUT_NAME,CUTS.DESCRIPTION from CUTS left join CART \
       on CUTS.CART_NUMBER=CART.NUMBER where CART.NUMBER is null";
  q=new QSqlQuery(sql);
  while(q->next()) {
    printf("  Cut %s [%s] is orphaned.\n",
	   (const char *)q->value(0).toString(),
	   (const char *)q->value(1).toString());
    //
    // Try to repair it
    //
    sql=QString().sprintf("select NUMBER from CART where NUMBER=%d",
			  q->value(0).toString().left(6).toUInt());
    q1=new QSqlQuery(sql);
    if(q1->first()) {
      printf("  Repair it (y/N)?");
      if(UserResponse()) {
	//
	// FIXME: Regen Cart Data
	//
	sql=QString().
	  sprintf("update CUTS set CART_NUMBER=%u where CUT_NAME=\"%s\"",
		  q1->value(0).toUInt(),
		  (const char *)q->value(0).toString());
	q2=new QSqlQuery(sql);
	delete q2;
	delete q1;
	printf("\n");
	continue;
      }
    }
    printf("\n");
    delete q1;

    //
    // Try to recover audio
    //
    file=new QFileInfo(RDCut::pathName(q->value(0).toString()));
    if(!file->exists()) {
      printf("  Clear it (y/N)?");
      if(UserResponse()) {
	sql=QString().sprintf("delete from CUTS where CUT_NAME=\"%s\"",
			      (const char *)q->value(0).toString());
	q1=new QSqlQuery(sql);
	delete q1;
      }
    }
    else {
      if(db_dump_cuts_dir.isEmpty()) {
	printf("  Rerun rddbcheck with the --dump-cuts-dir= switch to fix.\n");
      }
      else {
	printf("  Clear it (y/N)?");
	if(UserResponse()) {
	  system("mv "+file->filePath()+" "+db_dump_cuts_dir+"/");
	  sql=QString().sprintf("delete from CUTS where CUT_NAME=\"%s\"",
				(const char *)q->value(0).toString());
	  q1=new QSqlQuery(sql);
	  delete q1;
	  printf("  Saved audio in \"%s/%s\"\n",(const char *)db_dump_cuts_dir,
		 (const char *)file->fileName());
	}
      }
    }
    printf("\n");
    delete file;
    file=NULL;
  }
  delete q;
}


void MainObject::CheckOrphanedAudio() const
{
  QDir dir(db_config->audioRoot());
  QStringList list=dir.entryList("??????_???.wav",QDir::Files);
  for(unsigned i=0;i<list.size();i++) {
    bool ok=false;
    list[i].left(6).toUInt(&ok);
    if(ok) {
      list[i].mid(7,3).toInt(&ok);
      if(ok) {
	QString sql=QString().sprintf("select CUT_NAME from CUTS \
                                       where CUT_NAME=\"%s\"",
				      (const char *)list[i].left(10));
	QSqlQuery *q=new QSqlQuery(sql);
	if(!q->first()) {
	  printf("  File \"%s/%s\" is orphaned.\n",
		 (const char *)db_config->audioRoot(),(const char *)list[i]);
	  if(db_dump_cuts_dir.isEmpty()) {
	    printf(
	     "  Rerun rddbcheck with the --dump-cuts-dir= switch to fix.\n\n");
	  }
	  else {
	    printf("  Move to \"%s\" (y/N)? ",(const char *)db_dump_cuts_dir);
	    if(UserResponse()) {
	      system(QString().sprintf("mv %s/%s %s/",
				       (const char *)db_config->audioRoot(),
				       (const char *)list[i],
				       (const char *)db_dump_cuts_dir));
	      printf("  Saved audio in \"%s/%s\"\n",(const char *)db_dump_cuts_dir,
		     (const char *)list[i]);
	    }
	  }
	}
	delete q;
      }
    }
  }
}


void MainObject::ValidateAudioLengths() const
{
  QString sql;
  QSqlQuery *q;
  RDWaveFile *wave=NULL;

  sql="select CUT_NAME,CART_NUMBER,LENGTH from CUTS order by CART_NUMBER";
  q=new QSqlQuery(sql);
  while(q->next()) {
    if(q->value(2).toInt()>0) {
      wave=new RDWaveFile(RDCut::pathName(q->value(0).toString()));
      if(wave->openWave()) {
	if((int)wave->getExtTimeLength()<(q->value(2).toInt()-100)) {
	  SetCutLength(q->value(0).toString(),wave->getExtTimeLength());
	}
      }
      else {
	SetCutLength(q->value(0).toString(),0);
      }
      delete wave;
    }
  }
  delete q;
}


void MainObject::Rehash(const QString &arg) const
{
  QString sql;
  QSqlQuery *q;
  unsigned cartnum;
  bool ok=false;

  if(arg.lower()=="all") {
    sql=QString("select NUMBER from CART where ")+
      QString().sprintf("TYPE=%d ",RDCart::Audio)+
      "order by NUMBER";
    q=new QSqlQuery(sql);
    while(q->next()) {
      RehashCart(q->value(0).toUInt());
    }
    delete q;
    return;
  }
  cartnum=arg.toUInt(&ok);
  if(ok&&(cartnum>0)&&(cartnum<=RD_MAX_CART_NUMBER)) {
    RehashCart(cartnum);
    return;
  }
  RDCut *cut=new RDCut(arg);
  if(cut->exists()) {
    RehashCut(arg);
  }
  delete cut;
}


void MainObject::RehashCart(unsigned cartnum) const
{
  RDCart *cart=new RDCart(cartnum);
  if(cart->exists()) {
    if(cart->type()==RDCart::Audio) {
      QString sql=QString("select CUT_NAME from CUTS where ")+
	QString().sprintf("CART_NUMBER=%u ",cartnum)+
	"order by CUT_NAME";
      QSqlQuery *q=new QSqlQuery(sql);
      while(q->next()) {
	RehashCut(q->value(0).toString());
      }
      delete q;
    }
  }
  else {
    printf("  Cart %06u does not exist.\n",cartnum);
  }
}


void MainObject::RehashCut(const QString &cutnum) const
{
  QString hash=RDSha1Hash(RDCut::pathName(cutnum),true);
  if(hash.isEmpty()) {
    printf("  Unable to generate hash for \"%s\"\n",
	    (const char *)RDCut::pathName(cutnum));
  }
  else {
    RDCut *cut=new RDCut(cutnum);
    if(cut->exists()) {
      if(cut->sha1Hash().isEmpty()) {
	cut->setSha1Hash(hash);
      }
      else {
	if(cut->sha1Hash()!=hash) {
	  RDCart *cart=new RDCart(RDCut::cartNumber(cutnum));
	  printf("  Cut %d [%s] in cart %06u [%s] has inconsistent SHA1 hash.  Fix? (y/N) ",
		 cut->cutNumber(),
		 (const char *)cut->description(),
		 cart->number(),
		 (const char *)cart->title());
	  fflush(NULL);
	  if(UserResponse()) {
	    cut->setSha1Hash(hash);
	  }
	  delete cart;
	}
      }
    }
    else {
      printf("  Cut \"%s\" does not exist.\n",(const char *)cutnum);
    }
    delete cut;
  }
}


void MainObject::SetCutLength(const QString &cutname,int len) const
{
  QString sql;
  QSqlQuery *q;
  RDCut *cut=new RDCut(cutname);
  RDCart *cart=new RDCart(cut->cartNumber());

  printf("  Cut %d [%s] in cart %06u [%s] has invalid length.  Correct? (y/N) ",
	 cut->cutNumber(),
	 (const char *)cut->description(),
	 cart->number(),
	 (const char *)cart->title());
  fflush(NULL);
  if(UserResponse()) {
    fflush(NULL);
    sql=QString("update CUTS set ")+
      "START_POINT=0,"+
      QString().sprintf("END_POINT=%d,",len)+
      "FADEUP_POINT=-1,"+
      "FADEDOWN_POINT=-1,"+
      "SEGUE_START_POINT=-1,"+
      "SEGUE_END_POINT=-1,"+
      "TALK_START_POINT=-1,"+
      "TALK_END_POINT=-1,"+
      "HOOK_START_POINT=-1,"+
      "HOOK_END_POINT=-1,"+
      "PLAY_GAIN=0,"+
      QString().sprintf("LENGTH=%d where ",len)+
      "CUT_NAME=\""+RDEscapeString(cutname)+"\"";
    q=new QSqlQuery(sql);
    delete q;
    cart->updateLength();
    cart->resetRotation();
  }
  delete cart;
  delete cut;
}


void MainObject::CleanTables(const QString &ext,QSqlQuery *table_q,
			     QSqlQuery *name_q) const
{
  QString sql;
  QSqlQuery *q1;

  table_q->seek(-1);
  while(table_q->next()) {
    if(!IsTableLinked(name_q,ext,table_q->value(0).toString())) {
      printf("  Table %s is orphaned -- delete (y/N)? ",
	     (const char *)RDEscapeString(table_q->value(0).toString()));
      fflush(NULL);
      if(UserResponse()) {
	sql=QString().sprintf("drop table %s",
		  (const char *)RDEscapeString(table_q->value(0).toString()));
	q1=new QSqlQuery(sql);
	delete q1;
      }
    }
  }
}


void MainObject::CleanTables(const QString &ext,QSqlQuery *table_q) const
{
  QString sql;
  QSqlQuery *q1;

  table_q->seek(-1);
  while(table_q->next()) {
    if(table_q->value(0).toString().right(ext.length())==ext) {
      printf("  Table %s is orphaned -- delete (y/N)? ",
	     (const char *)table_q->value(0).toString());
      fflush(NULL);
      if(UserResponse()) {
	sql=QString().sprintf("drop table %s",
		  (const char *)RDEscapeString(table_q->value(0).toString()));
	q1=new QSqlQuery(sql);
	delete q1;
      }
    }
  }
}


bool MainObject::IsTableLinked(QSqlQuery *name_q,const QString &ext,
			       const QString &table) const
{
  QString tablename;

  if(table.right(ext.length())!=ext) {
    return true;
  }
  name_q->seek(-1);
  while(name_q->next()) {
    tablename=name_q->value(0).toString()+"_"+ext;
    tablename.replace(" ","_");
    if(tablename==table) {
      return true;
    }
  }
  return false;
}


bool MainObject::UserResponse() const
{
  char c=0;

  if(db_yes) {
    printf("y\n");
    return true;
  }
  if(db_no) {
    printf("n\n");
    return false;
  }
  while((c!='y')&&(c!='Y')&&(c!='n')&&(c!='N')) {
    scanf("%c",&c);
    if((c=='y')||(c=='Y')) {
      scanf("%c",&c);
      return true;
    }
    if(c=='\n') {
      return false;
    }
  }
  scanf("%c",&c);
  return false;
}


bool MainObject::CopyFile(const QString &destfile,const QString &srcfile) const
{
  int src_fd=-1;
  struct stat src_stat;
  int dest_fd=-1;
  struct stat dest_stat;
  int n;
  int blksize;
  char *data=NULL;

  if((src_fd=open(srcfile,O_RDONLY))<0) {
    return false;
  }
  fstat(src_fd,&src_stat);
  mode_t mask=umask(S_IRWXO);
  if((dest_fd=open(destfile,O_WRONLY|O_CREAT|O_TRUNC,
		   S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP))<0) {
    close(src_fd);
    return false;
  }
  umask(mask);
  fstat(dest_fd,&dest_stat);
  blksize=src_stat.st_blksize;
  if(dest_stat.st_blksize<blksize) {
    blksize=dest_stat.st_blksize;
  }
  data=(char *)malloc(blksize);
  while((n=read(src_fd,data,blksize))!=0) {
    write(dest_fd,data,n);
  }
  free(data);
  fchown(dest_fd,150,150);  // FIXME: do name lookup!
  close(dest_fd);
  close(src_fd);
  

  return true;
}
