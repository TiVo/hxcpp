import FilenameAndChecksum;

// A cache of files and their md5 hashes.  We cache these so we
// don't compute the md5 hash for the same file mulitple times.
class FileChecksumCache
{
   var mCache:Hash<FilenameAndChecksum>;

   public function new()
   {
      mCache = new Hash<FilenameAndChecksum>();
   }

   // Given a filename, return the filename and md5 hash for it.
   public function ChecksumFile(filename:String):FilenameAndChecksum
   {
      // Simplify stupid filenames...
      filename = StringTools.replace(filename, '//', '/');
      while (StringTools.startsWith(filename, "./")) {
         filename = filename.substr(2);
      }
      var re = ~/(.*)\/[^\/]+\/\.\.\/(.*)/;
      while (re.match(filename)) {
         filename = re.matched(1) + "/" + re.matched(2);
      }

      if (mCache.exists(filename)) {
         return mCache.get(filename);
      }

      // The Options.txt is generally a *good* thing to depend upon.
      // However, it also defines OBJDIR which is *different* for
      // every target.  So, we scrub the contents before computing an
      // MD5 hash on it.
      if (StringTools.endsWith(filename, "/Options.txt")) {
         var optionsText = sys.io.File.getContent(filename);
         var re = ~/(.*) -DOBJDIR="[^"]+" (.*)/;
         if (re.match(optionsText)) {
            optionsText = re.matched(1) + " " + re.matched(2);
         }
         var md5HashString = haxe.crypto.Md5.encode(optionsText);
         var newChecksum = new FilenameAndChecksum("Options.txt", md5HashString);
         mCache.set(filename, newChecksum);
         return newChecksum;
      } else {
         var contents = sys.io.File.getBytes(filename);
         var md5Hash = haxe.crypto.Md5.make(contents);
         var newChecksum = new FilenameAndChecksum(filename, md5Hash.toHex());
         mCache.set(filename, newChecksum);
         return newChecksum;
      }
   }
}
