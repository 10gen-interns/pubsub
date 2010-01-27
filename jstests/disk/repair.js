var baseName = "jstests_disk_repair";

port = allocatePorts( 1 )[ 0 ];
dbpath = "/data/db/" + baseName + "/";
repairpath = dbpath + "repairDir/"

resetDbpath( dbpath );
resetDbpath( repairpath );

m = startMongoProgram( "mongod", "--port", port, "--dbpath", dbpath, "--repairpath", repairpath, "--nohttpinterface", "--bind_ip", "127.0.0.1" );
db = m.getDB( baseName );
db[ baseName ].save( {} );
db.runCommand( {repairDatabase:1, backupOriginalFiles:true} );

files = listFiles( dbpath );
for( f in files ) {
    assert( ! new RegExp( "^" + dbpath + "backup_" ).test( files[ f ].name ), "backup dir in dbpath" );
}
