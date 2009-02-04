// query.cpp

/**
*    Copyright (C) 2008 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "stdafx.h"
#include "query.h"
#include "pdfile.h"
#include "jsobj.h"
#include "../util/builder.h"
#include <time.h>
#include "introspect.h"
#include "btree.h"
#include "../util/lruishmap.h"
#include "javajs.h"
#include "json.h"
#include "repl.h"
#include "replset.h"
#include "scanandorder.h"
#include "security.h"
#include "curop.h"
#include "commands.h"

namespace mongo {

    /* We cut off further objects once we cross this threshold; thus, you might get
       a little bit more than this, it is a threshold rather than a limit.
    */
    const int MaxBytesToReturnToClientAtOnce = 4 * 1024 * 1024;

//ns->query->DiskLoc
    LRUishMap<BSONObj,DiskLoc,5> lrutest(123);

    int nextCursorId = 1;
    extern bool useCursors;

    void appendElementHandlingGtLt(BSONObjBuilder& b, BSONElement& e);

    int matchDirection( const BSONObj &index, const BSONObj &sort ) {
        int direction = 0;
        BSONObjIterator i( index );
        BSONObjIterator s( sort );
        while ( 1 ) {
            BSONElement ie = i.next();
            BSONElement se = s.next();
            if ( ie.eoo() ) {
                if ( !se.eoo() )
                    return 0;
                return direction;
            }
            if ( strcmp( ie.fieldName(), se.fieldName() ) != 0 )
                return 0;

            int d = ie.number() == se.number() ? 1 : -1;
            if ( direction == 0 )
                direction = d;
            else if ( direction != d )
                return 0;
        }
    }

    /* todo: _ use index on partial match with the query

       parameters
         query - the query, e.g., { name: 'joe' }
         order - order by spec, e.g., { name: 1 } 1=ASC, -1=DESC
         simpleKeyMatch - set to true if the query is purely for a single key value
                          unchanged otherwise.
    */
    auto_ptr<Cursor> getIndexCursor(const char *ns, const BSONObj& query, const BSONObj& order, bool *simpleKeyMatch, bool *isSorted, BSONElement *hint) {
        NamespaceDetails *d = nsdetails(ns);
        if ( d == 0 ) return auto_ptr<Cursor>();

        if ( hint && !hint->eoo() ) {
            /* todo: more work needed.  doesn't handle $lt & $gt for example.
                     waiting for query optimizer rewrite (see queryoptimizer.h) before finishing the work.
            */
            if( hint->type() == String ) {
                string hintstr = hint->valuestr();
                for (int i = 0; i < d->nIndexes; i++ ) {
                    IndexDetails& ii = d->indexes[i];
                    if ( ii.indexName() == hintstr ) {
                        BSONObj startKey = ii.getKeyFromQuery(query);
                        int direction = 1;
                        if ( simpleKeyMatch )
                            *simpleKeyMatch = query.nFields() == startKey.nFields();
                        if ( isSorted ) *isSorted = false;
                        return auto_ptr<Cursor>(
                            new BtreeCursor(ii, startKey, direction, query));
                    }
                }
            }
            else if( hint->type() == Object ) { 
                BSONObj hintobj = hint->embeddedObject();
                for (int i = 0; i < d->nIndexes; i++ ) {
                    IndexDetails& ii = d->indexes[i];
                    if( ii.keyPattern().woCompare(hintobj) == 0 ) {
                        BSONObj startKey = ii.getKeyFromQuery(query);
                        int direction = 1;
                        if ( simpleKeyMatch )
                            *simpleKeyMatch = query.nFields() == startKey.nFields();
                        if ( isSorted ) *isSorted = false;
                        return auto_ptr<Cursor>(
                            new BtreeCursor(ii, startKey, direction, query));
                    }
                }
            }
            else { 
                uasserted("bad hint object");
            }
            uasserted("hint index not found");
        }

        if ( !order.isEmpty() ) {
            // order by
            for (int i = 0; i < d->nIndexes; i++ ) {
                BSONObj idxInfo = d->indexes[i].info.obj(); // { name:, ns:, key: }
                assert( strcmp(ns, idxInfo.getStringField("ns")) == 0 );
                BSONObj idxKey = idxInfo.getObjectField("key");
                int direction = matchDirection( idxKey, order );
                if ( direction != 0 ) {
                    DEV out() << " using index " << d->indexes[i].indexNamespace() << '\n';
                    if ( isSorted )
                        *isSorted = true;

                    return auto_ptr<Cursor>(new BtreeCursor(d->indexes[i], emptyObj, direction, query));
                }
            }
        }

        set<string> queryFields;
        query.getFieldNames(queryFields);
        // regular query without order by
        for (int i = 0; i < d->nIndexes; i++ ) {
            BSONObj idxInfo = d->indexes[i].info.obj(); // { name:, ns:, key: }
            BSONObj idxKey = idxInfo.getObjectField("key");
            set<string> keyFields;
            idxKey.getFieldNames(keyFields);

            if ( keyFields == queryFields ) {
                BSONObj q = query.extractFieldsUnDotted(idxKey);
                assert(q.objsize() != 0); // guard against a seg fault if details is 0
                                                                                                               
                // Make sure 1st element of index will help us.                                    
                BSONElement e = q.firstElement();          
                /* regexp: only supported if form is /^text/ */
                if ( e.type() == RegEx && !e.simpleRegex() )                         
                    continue;                                                                      
                if ( e.type() == Object && getGtLtOp( e ) >= JSMatcher::opIN )                     
                    continue;                                                                      
                
                bool simple = true;                                                                
                BSONObjIterator it( q );                                                           
                while( simple && it.more() ) {                
                    BSONElement e = it.next();
                    if ( e.eoo() )
                        break;
                    if ( e.isNumber() || e.mayEncapsulate() || e.type() == RegEx )
                        simple = false;                                                            
                }
                DEV out() << "using index " << d->indexes[i].indexNamespace() << endl;
                if ( simple && simpleKeyMatch )
                    *simpleKeyMatch = true;
                return auto_ptr< Cursor >( new BtreeCursor( d->indexes[ i ], emptyObj, 1, query ) );
            }
        }
        DEV out() << "getIndexCursor fail " << ns << '\n';
        return auto_ptr<Cursor>();
    }

    /* ns:      namespace, e.g. <database>.<collection>
       pattern: the "where" clause / criteria
       justOne: stop after 1 match
    */
    int deleteObjects(const char *ns, BSONObj pattern, bool justOne, BSONObj *deletedId, bool god) {
        if ( strstr(ns, ".system.") && !god ) {
            /* note a delete from system.indexes would corrupt the db 
               if done here, as there are pointers into those objects in 
               NamespaceDetails.
            */
            if( strstr(ns, ".system.users") )
                ;
            else {
                out() << "ERROR: attempt to delete in system namespace " << ns << endl;
                return -1;
            }
        }

        int nDeleted = 0;
        BSONObj order;
        auto_ptr<Cursor> c = getIndexCursor(ns, pattern, order);
        if ( c.get() == 0 )
            c = theDataFileMgr.findAll(ns);

        if( !c->ok() )
            return nDeleted;

        JSMatcher matcher(pattern, c->indexKeyPattern());

        do {
            Record *r = c->_current();
            DiskLoc rloc = c->currLoc();
            BSONObj js(r);
            
            bool deep;
            if ( !matcher.matches(js, &deep) ) {
                c->advance(); // advance must be after noMoreMatches() because it uses currKey()
            }
            else {
                c->advance(); // must advance before deleting as the next ptr will die
                assert( !deep || !c->getsetdup(rloc) ); // can't be a dup, we deleted it!
                if ( !justOne )
                    c->noteLocation();

                theDataFileMgr.deleteRecord(ns, r, rloc);
                nDeleted++;
                if ( justOne ) {
                    if ( deletedId ) {
                        BSONElement e;
                        if( js.getObjectID( e ) ) {
                            BSONObjBuilder b;
                            b.append( e );
                            *deletedId = b.doneAndDecouple();
                        }
                    }
                    break;
                }
                c->checkLocation();
            }
        } while ( c->ok() );

        return nDeleted;
    }

    struct Mod {
        enum Op { INC, SET } op;
        const char *fieldName;
        double *ndouble;
        int *nint;
        void setn(double n) {
            if ( ndouble ) *ndouble = n;
            else *nint = (int) n;
        }
        double getn() {
            return ndouble ? *ndouble : *nint;
        }
        int type;
        static void getMods(vector<Mod>& mods, BSONObj from);
        static void applyMods(vector<Mod>& mods, BSONObj obj);
    };

    void Mod::applyMods(vector<Mod>& mods, BSONObj obj) {
        for ( vector<Mod>::iterator i = mods.begin(); i != mods.end(); i++ ) {
            Mod& m = *i;
            BSONElement e = obj.findElement(m.fieldName);
            if ( e.isNumber() ) {
                if ( m.op == INC ) {
                    e.setNumber( e.number() + m.getn() );
                    m.setn( e.number() );
                    // *m.n = e.number() += *m.n;
                } else {
                    e.setNumber( m.getn() ); // $set or $SET
                }
            }
        }
    }

    /* get special operations like $inc
       { $inc: { a:1, b:1 } }
       { $set: { a:77 } }
       NOTE: MODIFIES source from object!
    */
    void Mod::getMods(vector<Mod>& mods, BSONObj from) {
        BSONObjIterator it(from);
        while ( it.more() ) {
            BSONElement e = it.next();
            const char *fn = e.fieldName();
            if ( *fn == '$' && e.type() == Object &&
                    fn[4] == 0 ) {
                BSONObj j = e.embeddedObject();
                BSONObjIterator jt(j);
                Op op = Mod::SET;
                if ( strcmp("$inc",fn) == 0 ) {
                    op = Mod::INC;
                    // we rename to $SET instead of $set so that on an op like
                    //   { $set: {x:1}, $inc: {y:1} }
                    // we don't get two "$set" fields which isn't allowed
                    strcpy((char *) fn, "$SET");
                }
                while ( jt.more() ) {
                    BSONElement f = jt.next();
                    if ( f.eoo() )
                        break;
                    Mod m;
                    m.op = op;
                    m.fieldName = f.fieldName();
                    if ( f.isNumber() ) {
                        if ( f.type() == NumberDouble ) {
                            m.ndouble = (double *) f.value();
                            m.nint = 0;
                        }
                        else {
                            m.ndouble = 0;
                            m.nint = (int *) f.value();
                        }
                        mods.push_back( m );
                    }
                }
            }
        }
    }

    int __updateObjects(const char *ns, BSONObj updateobj, BSONObj &pattern, bool upsert, stringstream& ss, bool logop=false) {
        int profile = database->profile;

        if ( strstr(ns, ".system.") ) {
            if( strstr(ns, ".system.users") )
                ;
            else {
                out() << "\nERROR: attempt to update in system namespace " << ns << endl;
                ss << " can't update system namespace ";
                return 0;
            }
        }

        int nscanned = 0;
        {
            BSONObj order;
            auto_ptr<Cursor> c = getIndexCursor(ns, pattern, order);
            if ( c.get() == 0 )
                c = theDataFileMgr.findAll(ns);
            /* we check ok first so we don't bother building the matcher if we don't need to */
            if( c->ok() ) { 
                JSMatcher matcher(pattern, c->indexKeyPattern());
                do {
                    Record *r = c->_current();
                    nscanned++;
                    BSONObj js(r);
                    if ( !matcher.matches(js) ) {
                    }
                    else {
                        if ( logop ) {
                            BSONObjBuilder idPattern;
                            BSONElement id;
                            if ( js.getObjectID( id ) )
                                idPattern.append( id );
                            pattern = idPattern.doneAndDecouple();
                        }
                        
                        /* note: we only update one row and quit.  if you do multiple later,
                        be careful or multikeys in arrays could break things badly.  best
                        to only allow updating a single row with a multikey lookup.
                        */

                        if ( profile )
                            ss << " nscanned:" << nscanned;

                        /* look for $inc etc.  note as listed here, all fields to inc must be this type, you can't set some
                        regular ones at the moment. */
                        const char *firstField = updateobj.firstElement().fieldName();
                        if ( firstField[0] == '$' ) {
                            vector<Mod> mods;
                            Mod::getMods(mods, updateobj);
                            NamespaceDetailsTransient& ndt = NamespaceDetailsTransient::get(ns);
                            set<string>& idxKeys = ndt.indexKeys();
                            for ( vector<Mod>::iterator i = mods.begin(); i != mods.end(); i++ ) {
                                if ( idxKeys.count(i->fieldName) ) {
                                    uassert("can't $inc/$set an indexed field", false);
                                }
                            }

                            Mod::applyMods(mods, c->currLoc().obj());
                            if ( profile )
                                ss << " fastmod ";
                            if ( logop ) {
                                if ( mods.size() ) {
                                    logOp("u", ns, updateobj, &pattern, &upsert);
                                    return 5;
                                }
                            }
                            return 2;
                        }

                        theDataFileMgr.update(ns, r, c->currLoc(), updateobj.objdata(), updateobj.objsize(), ss);
                        return 1;
                    }
                    c->advance();
                } while( c->ok() );
            }
        }

        if ( profile )
            ss << " nscanned:" << nscanned;

        if ( upsert ) {
            if ( updateobj.firstElement().fieldName()[0] == '$' ) {
                /* upsert of an $inc. build a default */
                vector<Mod> mods;
                Mod::getMods(mods, updateobj);
                BSONObjBuilder b;
                BSONObjIterator i( pattern );
                while( i.more() ) {
                    BSONElement e = i.next();
                    if ( e.eoo() )
                        break;
                    // Presumably the number of mods is small, so this loop isn't too expensive.
                    for( vector<Mod>::iterator i = mods.begin(); i != mods.end(); ++i ) {
                        if ( strcmp( e.fieldName(), i->fieldName ) == 0 )
                            continue;
                        b.append( e );
                    }
                }
                for ( vector<Mod>::iterator i = mods.begin(); i != mods.end(); i++ )
                    b.append(i->fieldName, i->getn());
                BSONObj obj = b.done();
                theDataFileMgr.insert(ns, obj);
                if ( profile )
                    ss << " fastmodinsert ";
                if ( logOp )
                    logOp( "i", ns, obj );
                return 3;
            }
            if ( profile )
                ss << " upsert ";
            theDataFileMgr.insert(ns, updateobj);
            if ( logOp )
                logOp( "i", ns, updateobj );
            return 4;
        }
        return 0;
    }

    /* todo:
     _ smart requery find record immediately
     returns:
     2: we did applyMods() but didn't logOp()
     5: we did applyMods() and did logOp() (so don't do it again)
     (clean these up later...)
     */
    int _updateObjects(const char *ns, BSONObj updateobj, BSONObj pattern, bool upsert, stringstream& ss, bool logop=false) {
        return __updateObjects( ns, updateobj, pattern, upsert, ss, logop );
    }
        
    /* todo: we can optimize replication by just doing insert when an upsert triggers.
    */
    void updateObjects(const char *ns, BSONObj updateobj, BSONObj pattern, bool upsert, stringstream& ss) {
        int rc = __updateObjects(ns, updateobj, pattern, upsert, ss, true);
        if ( rc != 5 && rc != 0 && rc != 4 && rc != 3 )
            logOp("u", ns, updateobj, &pattern, &upsert);
    }

    int queryTraceLevel = 0;
    int otherTraceLevel = 0;

    int initialExtentSize(int len);

    bool runCommands(const char *ns, BSONObj& jsobj, stringstream& ss, BufBuilder &b, BSONObjBuilder& anObjBuilder, bool fromRepl, int queryOptions) {
        try {
            return _runCommands(ns, jsobj, ss, b, anObjBuilder, fromRepl, queryOptions);
        }
        catch ( AssertionException e ) {
            if ( !e.msg.empty() )
                anObjBuilder.append("assertion", e.msg);
        }
        ss << " assertion ";
        anObjBuilder.append("errmsg", "db assertion failure");
        anObjBuilder.append("ok", 0.0);
        BSONObj x = anObjBuilder.done();
        b.append((void*) x.objdata(), x.objsize());
        return true;
    }

    int nCaught = 0;

    void killCursors(int n, long long *ids) {
        int k = 0;
        for ( int i = 0; i < n; i++ ) {
            if ( ClientCursor::erase(ids[i]) )
                k++;
        }
        log() << "killCursors: found " << k << " of " << n << '\n';
    }

    BSONObj id_obj = fromjson("{\"_id\":ObjectId( \"000000000000000000000000\" )}");
    BSONObj empty_obj = fromjson("{}");

    /* { count: "collectionname"[, query: <query>] }
       returns -1 on error.
    */
    int runCount(const char *ns, BSONObj& cmd, string& err) {
        NamespaceDetails *d = nsdetails(ns);
        if ( d == 0 ) {
            err = "ns does not exist";
            return -1;
        }

        BSONObj query = cmd.getObjectField("query");
        
        set< string > fields;
        cmd.getObjectField("fields").getFieldNames( fields );

        if ( query.isEmpty() && fields.empty() ) {
            // count of all objects
            return (int) d->nrecords;
        }

        auto_ptr<Cursor> c;

        bool simpleKeyToMatch = false;
        c = getIndexCursor(ns, query, empty_obj, &simpleKeyToMatch);

        if ( c.get() ) {
            // TODO We could check if all fields in the key are in 'fields'
            if ( simpleKeyToMatch && fields.empty() ) {
                /* Here we only look at the btree keys to determine if a match, instead of looking
                   into the records, which would be much slower.
                   */
                int count = 0;
                BtreeCursor *bc = dynamic_cast<BtreeCursor *>(c.get());
                if ( c->ok() && !query.woCompare( bc->currKeyNode().key, BSONObj(), false ) ) {
                    BSONObj firstMatch = bc->currKeyNode().key;
                    count++;
                    while ( c->advance() ) {
                        if ( !firstMatch.woEqual( bc->currKeyNode().key ) )
                            break;
                        count++;
                    }
                }
                return count;
            }
        } else {
            c = findTableScan(ns, empty_obj);
        }

        int count = 0;
        auto_ptr<JSMatcher> matcher(new JSMatcher(query, c->indexKeyPattern()));
        while ( c->ok() ) {
            BSONObj js = c->current();
            bool deep;
            if ( !matcher->matches(js, &deep) ) {
            }
            else if ( !deep || !c->getsetdup(c->currLoc()) ) { // i.e., check for dups on deep items only
                bool match = true;
                for( set< string >::iterator i = fields.begin(); i != fields.end(); ++i ) {
                    if ( js.getFieldDotted( i->c_str() ).eoo() ) {
                        match = false;
                        break;
                    }
                }
                if ( match )
                    ++count;
            }
            c->advance();
        }
        return count;
    }

    /* This is for languages whose "objects" are not well ordered (JSON is well ordered).
       [ { a : ... } , { b : ... } ] -> { a : ..., b : ... }
    */
    inline BSONObj transformOrderFromArrayFormat(BSONObj order) {
        /* note: this is slow, but that is ok as order will have very few pieces */
        BSONObjBuilder b;
        char p[2] = "0";

        while ( 1 ) {
            BSONObj j = order.getObjectField(p);
            if ( j.isEmpty() )
                break;
            BSONElement e = j.firstElement();
            uassert("bad order array", !e.eoo());
            uassert("bad order array [2]", e.isNumber());
            b.append(e);
            (*p)++;
            uassert("too many ordering elements", *p <= '9');
        }

        return b.doneAndDecouple();
    }

    QueryResult* runQuery(Message& message, const char *ns, int ntoskip, int _ntoreturn, BSONObj jsobj,
                          auto_ptr< set<string> > filter, stringstream& ss, int queryOptions)
    {
        Timer t;
        
        log(2) << "runQuery: " << ns << jsobj << endl;

        int nscanned = 0;
        bool wantMore = true;
        int ntoreturn = _ntoreturn;
        if ( _ntoreturn < 0 ) {
            ntoreturn = -_ntoreturn;
            wantMore = false;
        }
        ss << "query " << ns << " ntoreturn:" << ntoreturn;
        {
            string s = jsobj.toString();
            strncpy(currentOp.query, s.c_str(), sizeof(currentOp.query)-1);
        }

        int n = 0;
        BufBuilder b(32768);
        BSONObjBuilder cmdResBuf;
        long long cursorid = 0;

        b.skip(sizeof(QueryResult));

        /* we assume you are using findOne() for running a cmd... */
        if ( ntoreturn == 1 && runCommands(ns, jsobj, ss, b, cmdResBuf, false, queryOptions) ) {
            n = 1;
        }
        else {

            AuthenticationInfo *ai = authInfo.get();
            uassert("unauthorized", ai->isAuthorized(database->name.c_str()));

            uassert("not master", isMaster() || (queryOptions & Option_SlaveOk));

            BSONElement hint;
            bool explain = false;
            bool _gotquery = false;
            BSONObj query;// = jsobj.getObjectField("query");
            {
                BSONElement e = jsobj.findElement("query");
                if ( !e.eoo() && (e.type() == Object || e.type() == Array) ) {
                    query = e.embeddedObject();
                    _gotquery = true;
                }
            }
            BSONObj order;
            {
                BSONElement e = jsobj.findElement("orderby");
                if ( !e.eoo() ) {
                    order = e.embeddedObjectUserCheck();
                    if ( e.type() == Array )
                        order = transformOrderFromArrayFormat(order);
                }
            }
            if ( !_gotquery && order.isEmpty() )
                query = jsobj;
            else {
                explain = jsobj.getBoolField("$explain");
                hint = jsobj.getField("$hint");
            }

            /* The ElemIter will not be happy if this isn't really an object. So throw exception
               here when that is true.
                (Which may indicate bad data from appserver?)
            */
            if ( query.objsize() == 0 ) {
                out() << "Bad query object?\n  jsobj:";
                out() << jsobj.toString() << "\n  query:";
                out() << query.toString() << endl;
                uassert("bad query object", false);
            }

            bool isSorted = false;
            auto_ptr<Cursor> c = getSpecialCursor(ns);

            if ( c.get() == 0 )
                c = getIndexCursor(ns, query, order, 0, &isSorted, &hint);
            if ( c.get() == 0 )
                c = findTableScan(ns, order, &isSorted);

            auto_ptr<ScanAndOrder> so;
            bool ordering = false;

            auto_ptr<JSMatcher> matcher(new JSMatcher(query, c->indexKeyPattern()));

            if ( !order.isEmpty() && !isSorted ) {
                ordering = true;
                ss << " scanAndOrder ";
                so = auto_ptr<ScanAndOrder>(new ScanAndOrder(ntoskip, ntoreturn,order));
                wantMore = false;
                //			scanAndOrder(b, c.get(), order, ntoreturn);
            }

            while ( c->ok() ) {
                BSONObj js = c->current();
                nscanned++;
                bool deep;
                if ( !matcher->matches(js, &deep) ) {
                }
                else if ( !deep || !c->getsetdup(c->currLoc()) ) { // i.e., check for dups on deep items only
                    // got a match.
                    assert( js.objsize() >= 0 ); //defensive for segfaults
                    if ( ordering ) {
                        // note: no cursors for non-indexed, ordered results.  results must be fairly small.
                        so->add(js);
                    }
                    else if ( ntoskip > 0 ) {
                        ntoskip--;
                    } else {
                        if ( explain ) {
                            n++;
                            if ( n >= ntoreturn && !wantMore )
                                break; // .limit() was used, show just that much.
                        }
                        else {
                            bool ok = fillQueryResultFromObj(b, filter.get(), js);
                            if ( ok ) n++;
                            if ( ok ) {
                                if ( (ntoreturn>0 && (n >= ntoreturn || b.len() > MaxBytesToReturnToClientAtOnce)) ||
                                        (ntoreturn==0 && (b.len()>1*1024*1024 || n>=101)) ) {
                                    /* if ntoreturn is zero, we return up to 101 objects.  on the subsequent getmore, there
                                    is only a size limit.  The idea is that on a find() where one doesn't use much results,
                                    we don't return much, but once getmore kicks in, we start pushing significant quantities.

                                    The n limit (vs. size) is important when someone fetches only one small field from big
                                    objects, which causes massive scanning server-side.
                                    */
                                    /* if only 1 requested, no cursor saved for efficiency...we assume it is findOne() */
                                    if ( wantMore && ntoreturn != 1 ) {
                                        if ( useCursors ) {
                                            c->advance();
                                            if ( c->ok() ) {
                                                // more...so save a cursor
                                                ClientCursor *cc = new ClientCursor();
                                                cc->c = c;
                                                cursorid = cc->cursorid;
                                                DEV out() << "  query has more, cursorid: " << cursorid << endl;
                                                //cc->pattern = query;
                                                cc->matcher = matcher;
                                                cc->ns = ns;
                                                cc->pos = n;
                                                cc->filter = filter;
                                                cc->originalMessage = message;
                                                cc->updateLocation();
                                            }
                                        }
                                    }
                                    break;
                                }
                            }
                        }
                    }
                }
                c->advance();
            } // end while

            if ( explain ) {
                BSONObjBuilder builder;
                builder.append("cursor", c->toString());
                builder.append("startKey", c->prettyStartKey());
                builder.append("endKey", c->prettyEndKey());
                builder.append("nscanned", nscanned);
                builder.append("n", ordering ? so->size() : n);
                if ( ordering )
                    builder.append("scanAndOrder", true);
                builder.append("millis", t.millis());
                BSONObj obj = builder.done();
                fillQueryResultFromObj(b, 0, obj);
                n = 1;
            } else if ( ordering ) {
                so->fill(b, filter.get(), n);
            }
            else if ( cursorid == 0 && (queryOptions & Option_CursorTailable) && c->tailable() ) {
                c->setAtTail();
                ClientCursor *cc = new ClientCursor();
                cc->c = c;
                cursorid = cc->cursorid;
                DEV out() << "  query has no more but tailable, cursorid: " << cursorid << endl;
                //cc->pattern = query;
                cc->matcher = matcher;
                cc->ns = ns;
                cc->pos = n;
                cc->filter = filter;
                cc->originalMessage = message;
                cc->updateLocation();
            }
        }

        QueryResult *qr = (QueryResult *) b.buf();
        qr->resultFlags() = 0;
        qr->len = b.len();
        ss << " reslen:" << b.len();
        //	qr->channel = 0;
        qr->setOperation(opReply);
        qr->cursorId = cursorid;
        qr->startingFrom = 0;
        qr->nReturned = n;
        b.decouple();

        int duration = t.millis();
        if ( (database && database->profile) || duration >= 100 ) {
            ss << " nscanned:" << nscanned << ' ';
            if ( ntoskip )
                ss << " ntoskip:" << ntoskip;
            if ( database && database->profile )
                ss << " \nquery: ";
            ss << jsobj.toString() << ' ';
        }
        ss << " nreturned:" << n;
        return qr;
    }

//int dump = 0;

    /* empty result for error conditions */
    QueryResult* emptyMoreResult(long long cursorid) {
        BufBuilder b(32768);
        b.skip(sizeof(QueryResult));
        QueryResult *qr = (QueryResult *) b.buf();
        qr->cursorId = 0; // 0 indicates no more data to retrieve.
        qr->startingFrom = 0;
        qr->len = b.len();
        qr->setOperation(opReply);
        qr->nReturned = 0;
        b.decouple();
        return qr;
    }

    QueryResult* getMore(const char *ns, int ntoreturn, long long cursorid) {
        BufBuilder b(32768);

        ClientCursor *cc = ClientCursor::find(cursorid);

        b.skip(sizeof(QueryResult));

        int resultFlags = 0;
        int start = 0;
        int n = 0;

        if ( !cc ) {
            DEV log() << "getMore: cursorid not found " << ns << " " << cursorid << endl;
            cursorid = 0;
            resultFlags = QueryResult::ResultFlag_CursorNotFound;
        }
        else {
            start = cc->pos;
            Cursor *c = cc->c.get();
            c->checkLocation();
            c->tailResume();
            while ( 1 ) {
                if ( !c->ok() ) {
                    if ( c->tailing() ) {
                        c->setAtTail();
                        break;
                    }
                    DEV log() << "  getmore: last batch, erasing cursor " << cursorid << endl;
                    bool ok = ClientCursor::erase(cursorid);
                    assert(ok);
                    cursorid = 0;
                    cc = 0;
                    break;
                }
                BSONObj js = c->current();

                bool deep;
                if ( !cc->matcher->matches(js, &deep) ) {
                }
                else {
                    //out() << "matches " << c->currLoc().toString() << ' ' << deep << '\n';
                    if ( deep && c->getsetdup(c->currLoc()) ) {
                        //out() << "  but it's a dup \n";
                    }
                    else {
                        bool ok = fillQueryResultFromObj(b, cc->filter.get(), js);
                        if ( ok ) {
                            n++;
                            if ( (ntoreturn>0 && (n >= ntoreturn || b.len() > MaxBytesToReturnToClientAtOnce)) ||
                                    (ntoreturn==0 && b.len()>1*1024*1024) ) {
                                c->advance();
                                if ( c->tailing() && !c->ok() )
                                    c->setAtTail();
                                cc->pos += n;
                                //cc->updateLocation();
                                break;
                            }
                        }
                    }
                }
                c->advance();
            }
            if ( cc )
                cc->updateLocation();
        }

        QueryResult *qr = (QueryResult *) b.buf();
        qr->len = b.len();
        qr->setOperation(opReply);
        qr->resultFlags() = resultFlags;
        qr->cursorId = cursorid;
        qr->startingFrom = start;
        qr->nReturned = n;
        b.decouple();

        return qr;
    }

} // namespace mongo
