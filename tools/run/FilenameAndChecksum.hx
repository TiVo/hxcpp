// Just a tuple of filename and checksum.
class FilenameAndChecksum
{
   public var mFilename:String;
   public var mChecksum:String;

   public function new(filename:String, checksum:String)
   {
      mFilename = filename;
      mChecksum = checksum;
   }
}
