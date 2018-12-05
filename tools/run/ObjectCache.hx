import haxe.io.Eof;
import haxe.io.Path;
import sys.FileSystem;
import sys.io.File;

// This class abstracts the objectCache we use during building.
// The object cache keeps a cache of .o files and their dependencies
// (complete with md5 hashes for them).  If a .o file is in the cache,
// we can pull it from there rather than recompiling it.
class ObjectCache
{
   var mDirName:String;

   public function new(dirName:String)
   {
      mDirName = dirName;
      if (!FileSystem.exists(mDirName)) {
         // If this fails, we've got a legitimate problem.
         DirectoryCreator.createDirectory(mDirName);
      }
      if (!FileSystem.isDirectory(mDirName)) {
         throw "Error: " + mDirName + " is not a directory!";
      }
   }

   public function makeFullDirectory(directory:String)
   {
      var path = new Path(directory);
      if (!FileSystem.exists(path.dir)) {
         makeFullDirectory(path.dir);
      }
      DirectoryCreator.createDirectory(directory);
   }

   // Attempt to pull fileName from the cache.  Returns true if it succeeds.
   public function pullFromCache(
         filename:String, cacheName:String, dependencies:Dependencies):Bool
   {
      var cachePath = mDirName + "/" + cacheName;
      if (!FileSystem.exists(cachePath)) {
         return false;
      }
      var dependFileName = cachePath + ".deps";
      if (!FileSystem.exists(dependFileName)) {
         return false;
      }
      var fileIn = File.read(dependFileName, false);

      var checksumHash = new Hash<String>();
      for (fileAndChecksum in dependencies) {
         checksumHash.set(fileAndChecksum.mFilename, fileAndChecksum.mChecksum);
      }

      // Must use try, readLine() throws exceptions at Eof! (Doh!)
      try {
         var re = ~/(.*): (.*)/;
         while (true) {
            var line = StringTools.trim(fileIn.readLine());
            if (re.match(line)) {
               var md5sum = re.matched(1);
               var filename = re.matched(2);
               if (!checksumHash.exists(filename) || 
                     checksumHash.get(filename) != md5sum) {
                  fileIn.close();
                  return false;
               }
            }
         }
      } catch (e:Eof) { 
         var contents = File.getBytes(cachePath);
         File.saveBytes(filename, contents);
         fileIn.close();
         return true;
      }
   }

   // Save fileName from the cache
   public function cacheFile(filename:String, cacheName:String, dependencies:Dependencies)
   {
      var path = new Path(mDirName + "/" + cacheName);
      if (!FileSystem.exists(path.dir)) {
         makeFullDirectory(path.dir);
      }
      var objTarget = path.toString();
      // Clean out old .o as we change dependency information for it.
      // We are just making sure that our cache is valid during every
      // step of updating it.
      if (FileSystem.exists(objTarget)) {
         FileSystem.deleteFile(objTarget);
      }
      var dependFileName = objTarget + ".deps";
      var fileOut = File.write(dependFileName, false);
      for (fileAndChecksum in dependencies) {
         fileOut.writeString(fileAndChecksum.mChecksum + ": " + fileAndChecksum.mFilename + "\n");
      }
      fileOut.close();

      // We make sure that the creation of the .o file in the cache is
      // atomic so we never get an invalid cache.
      var contents = File.getBytes(filename);
      var objTargetTemp = objTarget + "_";
      sys.io.File.saveBytes(objTargetTemp, contents);
      FileSystem.rename(objTargetTemp, objTarget);
   }
}

