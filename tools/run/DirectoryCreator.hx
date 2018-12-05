import sys.FileSystem;

class DirectoryCreator
{
    public static function createDirectory(dir : String)
    {
        try {
            FileSystem.createDirectory(dir);
        }
        catch (e : Dynamic) {
            // If the directory actually exists, then ignore the error
            if (!FileSystem.isDirectory(dir)) {
                Sys.stderr().writeString("Failed to create directory [" +
                                         dir + "]: " + e + "\n");
                throw e;
            }
        }
    }
}
