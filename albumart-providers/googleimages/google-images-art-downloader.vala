using GLib;

// This is the interface on DBus that you must implement

[DBus (name = "com.nokia.albumart.Provider")]
public interface Provider {
	public abstract void Fetch (string artist, string album, string uri);
}

// Sample implementation of com.nokia.albumart.Provider that uses Google 
// images's first result as album-art cover. There is of course no certainty 
// that the first result on Google images is indeed the album's cover. But it's
// a good estimate nonetheless.

public class GoogleImages : Object, Provider {

	public void Fetch (string artist, string album, string uri) {
		uint u = 0, hread = 0;
		string [] pieces = artist.split (" ", -1);
		string stitched = "";
		bool first = true;

		// Convert the album and artist into something that will work for Google images

		while (pieces[u] != null) {
			if (!first)
				stitched += "+";
			stitched += pieces[u];
			u++;
			first = false;
		}

		stitched += "+";

		u = 0;
		first = true;
		pieces = album.split (" ", -1);

		while (pieces[u] != null) {
			if (!first)
				stitched += "+";
			stitched += pieces[u];
			u++;
			first = false;
		}

		stitched += "+album+cover";

		// Start the query on Google images

		File google_search = File.new_for_uri ("http://images.google.com/images?q=" + Uri.escape_string (stitched, "+", false));

		try {
			char [] buffer = new char [40000];
			string asstring;

			// Fetch the first page

			InputStream stream = google_search.read (null);
			stream.read_all (buffer, 40000, out hread, null);
			buffer[hread] = 0;

			asstring = (string) buffer;

			// Find the first result

			string found = asstring.str ("http://tbn0.google.com/images?q=tbn");

			if (found != null) {

				StringBuilder url = new StringBuilder ();
				long y = found.len();
				int i = 0;
				
				while (found[i] != ' ' && i < y) {
					url.append_unichar (found[i]);
					i++;
				}

				string cache_path;

				string cache_dir = Path.build_filename (Environment.get_home_dir(),
								  ".album_art",
								  null);

				// Define cache path = ~/.album_art/MD5 (down (albumartist)).jpeg

				cache_path = Path.build_filename (Environment.get_home_dir(),
								  ".album_art",
								  Checksum.compute_for_string (
										   ChecksumType.MD5, 
										   (artist + " " + album).down (), 
										   -1) + ".jpeg",
								  null);

				// Make sure the directory .album_arts is available

				DirUtils.create_with_parents (cache_dir, 0770);

				File online_image = File.new_for_uri (url.str);
				File cache_image = File.new_for_path (cache_path);

				// Copy from Google images to local cache

				online_image.copy (cache_image, 
								   FileCopyFlags.NONE, 
								   null, 
								   null);
			}

		} catch (GLib.Error error) {
		}
	}
}

void main () 
{
	MainLoop loop = new MainLoop (null, false);

	try {
		var conn = DBus.Bus.get (DBus.BusType. SESSION);

		dynamic DBus.Object bus = conn.get_object (
			"org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus");

		// try to register service in session bus

		uint request_name_result = bus.request_name ("com.nokia.albumart.GoogleImages", (uint) 0);

		if (request_name_result == DBus.RequestNameReply.PRIMARY_OWNER) {

			// start server

			var server = new GoogleImages ();
			conn.register_object ("/com/nokia/albumart/GoogleImages", server);

			loop.run ();
		}
	} catch (Error foo) {
		stderr.printf("Oops %s\n", foo.message);
	}
}

