sh = function() { return "try sh.help();" }


sh._checkMongos = function() {
    var x = db.runCommand( "ismaster" );
    if ( x.msg != "isdbgrid" )
        throw "not connected to a mongos"
}

sh._checkFullName = function( fullName ) {
    assert( fullName , "neeed a full name" )
    assert( fullName.indexOf( "." ) > 0 , "name needs to be fully qualified <db>.<collection>'" )
}

sh._adminCommand = function( cmd ) {
    sh._checkMongos();
    var res = db.getSisterDB( "admin" ).runCommand( cmd );

    if ( res == null || ! res.ok ) {
        print( "command failed: " + tojson( res ) )
    }

    return res;
}

sh.help = function() {
    print( "\tsh.enableSharding(dbname)                 enables sharding on the database dbname" )
    print( "\tsh.shardCollection(fullName,key,unique)   shards the collection" );
    print( "\tsh.splitFind(fullName,find)               splits the chunk that find is in at the median" );
    print( "\tsh.splitAt(fullName,middle)           splits the chunk that middle is in at middle" );
    
    print( "\tsh.status()                               prints a general overview of the cluster" )
}

sh.status = function( verbose , configDB ) { 
    // TODO: move the actual commadn here
    printShardingStatus( configDB , verbose );
}

sh.enableSharding = function( dbname ) { 
    assert( dbname , "need a valid dbname" )
    sh._adminCommand( { enableSharding : dbname } )
}

sh.shardCollection = function( fullName , key , unique ) {
    sh._checkFullName( fullName )
    assert( key , "need a key" )
    assert( typeof( key ) == "object" , "key needs to be an object" )
    
    var cmd = { shardCollection : fullName , key : key }
    if ( unique ) 
        cmd.unique = true;

    sh._adminCommand( cmd )
}


sh.splitFind = function( fullName , find ) {
    sh._checkFullName( fullName )
    sh._adminCommand( { split : fullName , find : find } )
}

sh.splitAt = function( fullName , middle ) {
    sh._checkFullName( fullName )
    sh._adminCommand( { split : fullName , middle : middle } )
}
